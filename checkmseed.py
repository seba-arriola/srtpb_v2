#!/usr/bin/env python

# ***********************************************************************************
# *    Copyright (C) by Ran Novitsky Nof                                            *
# *                                                                                 *
# *    This file is part of srtpb                                                   *
# *                                                                                 *
# *    srtpb is free software: you can redistribute it and/or modify                *
# *    it under the terms of the GNU Lesser General Public License as published by  *
# *    the Free Software Foundation, either version 3 of the License, or            *
# *    (at your option) any later version.                                          *
# ***********************************************************************************

# checkmseed.py - Inspect MiniSEED files to plan srtpb.py replays.
#
# Reads only the headers of the given MiniSEED files (fast, no data loaded) and reports:
#   * global earliest start / latest end of the whole file set
#   * a per-channel table of start/end times, sampling rate, number of samples and segments
#   * band summaries (L*, B*, H*, ...) with their time coverage and sample rates
#   * the time segments where the set of active channels changes (like a "who is streaming when")
#   * recommended -ti/-tf values for srtpb.py so all channels are active during the replay

import argparse
import sys

from obspy import read, UTCDateTime


def fmt_time(t, with_date=True):
    """Format a UTCDateTime compactly, millisecond precision."""
    s = t.strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3] if with_date else t.strftime('%H:%M:%S.%f')[:-3]
    return s


def band_of(trace_id):
    """Band code of a channel id (first letter of the channel, e.g. L, B, H)."""
    return trace_id.split('.')[-1][0]


def rate_label(rates):
    """Human readable label for a set of sampling rates."""
    if not rates:
        return ''
    rs = sorted(rates)
    if len(rs) == 1:
        return '%g Hz' % rs[0]
    return '/'.join('%g' % r for r in rs) + ' Hz'


def main(args):
    # ------------------------------------------------------------------ read headers
    traces = []
    for f in args.path:
        try:
            st = read(f, headonly=True)
        except Exception as msg:
            print('Error reading %s: %s' % (f, msg.args[0] if getattr(msg, 'args', None) else msg),
                  file=sys.stderr)
            continue
        traces.extend(st)
        if args.verbose:
            print('%-40s %6d traces' % (f, len(st)), file=sys.stderr)
    if not traces:
        raise SystemExit('No data could be found.')

    # group by channel id (NET.STA.LOC.CHAN)
    byid = {}
    for t in traces:
        byid.setdefault(t.id, []).append(t)

    # ------------------------------------------------------------- global statistics
    gstart = min(min(tt.stats.starttime for tt in tl) for tl in byid.values())
    gend = max(max(tt.stats.endtime for tt in tl) for tl in byid.values())

    print('=' * 96)
    print('MiniSEED overview: %d traces, %d distinct channels, %d distinct stations'
          % (len(traces), len(byid),
             len(set(t.stats.network + '.' + t.stats.station for t in traces))))
    print('=' * 96)
    print('Global earliest start : %s' % fmt_time(gstart))
    print('Global latest end     : %s' % fmt_time(gend))
    print('Total span            : %.1f minutes' % ((gend - gstart) / 60.0))
    print('')

    # ---------------------------------------------------------- band summary
    bands = {}
    for cid, tl in byid.items():
        b = band_of(cid)
        info = bands.setdefault(b, {'rate': set(), 'start': tl[0].stats.starttime,
                                    'end': tl[0].stats.endtime, 'n': 0})
        info['rate'].add(tl[0].stats.sampling_rate)
        info['start'] = min(info['start'], min(tt.stats.starttime for tt in tl))
        info['end'] = max(info['end'], max(tt.stats.endtime for tt in tl))
        info['n'] += 1
    print('Band summary (first letter of channel):')
    for b in sorted(bands):
        i = bands[b]
        print('  %s* : %4d channels | %s .. %s | %s'
              % (b, i['n'], fmt_time(i['start']), fmt_time(i['end']), rate_label(i['rate'])))
    print('')

    # ----------------------------------------------------------- per-channel table
    print('Per-channel start/end times (sorted by start):')
    rows = []
    for cid, tl in byid.items():
        st = min(tt.stats.starttime for tt in tl)
        et = max(tt.stats.endtime for tt in tl)
        rate = tl[0].stats.sampling_rate
        npts = sum(int(tt.stats.npts) for tt in tl)
        rows.append((st, et, cid, rate, npts, len(tl)))
    rows.sort(key=lambda r: (r[0], r[2]) if not args.sort_id else (r[2], r[0]))
    print('  %-24s %-20s %-20s %-10s %8s %6s'
          % ('ID', 'Start', 'End', 'Rate', 'nSamp', 'Segs'))
    for st, et, cid, rate, npts, nseg in rows:
        print('  %-24s %-20s %-20s %-10g %8d %6d' % (cid, fmt_time(st), fmt_time(et), rate, npts, nseg))
    print('')

    # --------------------------------------------------- active-channel phases
    # build channel start/end events
    dict_rate = dict((cid, tl[0].stats.sampling_rate) for cid, tl in byid.items())
    events = []  # (time, kind, cid, rate)
    for cid, tl in byid.items():
        st = min(tt.stats.starttime for tt in tl)
        et = max(tt.stats.endtime for tt in tl)
        events.append((st, 1, cid, dict_rate[cid]))
        events.append((et, -1, cid, dict_rate[cid]))
    events.sort(key=lambda e: e[0])

    segs = []  # (start, end, set_of_active_ids)
    active = set()
    prev = None
    i = 0
    n = len(events)
    while i < n:
        t = events[i][0]
        if prev is not None and t > prev and active:
            segs.append((prev, t, set(active)))
        # apply all events that share this exact time
        while i < n and events[i][0] == t:
            _, delta, cid, _ = events[i]
            if delta == 1:
                active.add(cid)
            else:
                active.discard(cid)
            i += 1
        prev = t

    def seg_label(act):
        bb = {}
        for cid in act:
            bb.setdefault(band_of(cid), set()).add(dict_rate[cid])
        return ' + '.join('%s* (%s)' % (b, rate_label(sorted(bb[b]))) for b in sorted(bb))

    def examples(act, limit=4):
        ids = sorted(act)
        extra = ' (+%d more)' % (len(ids) - limit) if len(ids) > limit else ''
        return ', '.join(ids[:limit]) + extra

    # group consecutive segments into phases with the same band signature;
    # within a phase, track the max channel count and the channels active at
    # the phase start
    phases = []  # [start, end, label, max_count, channels_at_start]
    for st, et, act in segs:
        lb = seg_label(act)
        if phases and phases[-1][2] == lb:
            phases[-1][1] = et
            phases[-1][3] = max(phases[-1][3], len(act))
        else:
            phases.append([st, et, lb, len(act), sorted(act)[:4]])

    if not args.no_segments:
        print('Active-channel phases (grouped by band signature):')
        print('  %-13s %-13s %6s  %-28s  %s'
              % ('Start', 'End', 'MaxCh', 'Active bands', 'Channels active at phase start'))
        for st, et, lb, mx, first in phases:
            print('  %-13s %-13s %6d  %-28s  %s'
                  % (fmt_time(st, False), fmt_time(et, False), mx, lb, examples(first)))
        print('')

    # --------------------------------------------------- recommendations for srtpb.py
    starts = [min(tt.stats.starttime for tt in tl) for tl in byid.values()]
    ends = [max(tt.stats.endtime for tt in tl) for tl in byid.values()]
    overlap_start = max(starts)
    overlap_end = min(ends)
    print('Recommendations for srtpb.py (times are UTC, same day as the data):')
    if overlap_start < overlap_end:
        print('  Window where ALL %d channels are active: %s .. %s'
              % (len(byid), fmt_time(overlap_start), fmt_time(overlap_end)))
        print('    -> srtpb.py -s 1.0 -m -ti %s -tf %s ...'
              % (overlap_start.strftime('%H:%M:%S'), overlap_end.strftime('%H:%M:%S')))
    else:
        print('  No single window covers all channels (data is too fragmented).')
    best = max(phases, key=lambda m: m[3])
    print('  Busiest phase (%d of %d channels active): %s .. %s'
          % (best[3], len(byid), fmt_time(best[0]), fmt_time(best[1])))
    print('')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='Inspect MiniSEED files (headers only) to plan srtpb.py replays.')
    parser.add_argument('path', metavar='PATH', nargs='+',
                        help='Path of data (use quotes for "PATH" if using regex)')
    parser.add_argument('--no-segments', action='store_true',
                        help='Skip the active-channel segment table')
    parser.add_argument('-c', '--sort-channel', dest='sort_id', action='store_true',
                        help='Sort the per-channel table by channel ID instead of start time')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='Verbose (report per-file trace counts)')
    args = parser.parse_args(sys.argv[1:])
    main(args)