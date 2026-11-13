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
# *                                                                                 *
# *    This program is distributed in the hope that it will be useful,              *
# *    but WITHOUT ANY WARRANTY; without even the implied warranty of               *
# *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                *
# *    GNU Lesser General Public License for more details.                          *
# *                                                                                 *
# *    You should have received a copy of the GNU Lesser General Public License     *
# *    along with this program.  If not, see <http://www.gnu.org/licenses/>.        *
# ***********************************************************************************

# By Ran Novitsky Nof @ BSL, 2015
# ran.nof@gmail.com

# This CLI is a thin front-end for the C replay engine (srtpbc).  The engine
# reads the MiniSEED data, slices it into PacketTimeInterval windows, packs
# STEIM2 records and sends them to a DataLink server in real time.

import argparse
import glob
import os
import shutil
import subprocess
import sys
import textwrap

from obspy import UTCDateTime


MAXProcs = (os.cpu_count() or 2) - 1
if not MAXProcs:
    MAXProcs = 1


def getTimeOfDay(s):
    '''a argparse function for -ti/-tf time of day parameters
    parsing a hh:mm:ss or hh:mm:ss.f string and returning (hh,mm,ss).
    '''
    try:
        p = s.strip().split(':')
        if len(p) != 3:
            raise ValueError
        hh = int(p[0])
        mm = int(p[1])
        ss = float(p[2])
        if not (0 <= hh <= 23 and 0 <= mm <= 59 and 0 <= ss < 60):
            raise ValueError
        return (hh, mm, ss)
    except Exception:
        msg = "Time of day must be in hh:mm:ss or hh:mm:ss.f format (e.g. 22:49:17)"
        raise argparse.ArgumentTypeError(msg)


def getLatency(s):
    '''a argparse function for latency commandline variable
    checking for a number or a file name, and return a number
    or a dictionary based on the file data.
    file should hold a list of NET.STA=latency for stations.
    stations not on the list will have 0 latency.
    '''
    try:
        return float(s)
    except Exception:
        pass
    if not os.path.exists(s):
        msg = "Can't find latency file %r" % s
        raise argparse.ArgumentTypeError(msg)
    try:
        f = open(s, 'r')
        lines = [l.strip().split('=') for l in f.readlines() if l.strip()]
        f.close()
        return dict([(sid.strip(), float(latency.strip()))
                     for sid, latency in lines])
    except Exception:
        msg = "File Format Error in %r." % s
        raise argparse.ArgumentTypeError(msg)


class getStartEndTimes(argparse.Action):
    '''A argparse action subclass.
       Called if a start-end time parameters are entered at command-line
    '''
    def __call__(self, parser, namespace, values, option_string=None):
        if len(values) > 2:
            raise argparse.ArgumentError(self, 'Need one or two parameters.')
        start = values[0]
        end = values[1] if len(values) == 2 else None
        setattr(namespace, 't', [start, end])


class getReadOptions(argparse.Action):
    '''A argparse action subclass.
       Called if options parameters are entered at command-line
    '''
    def __call__(self, parser, namespace, values, option_string=None):
        try:
            d = dict([v.split('=') for v in values])
        except ValueError:
            raise argparse.ArgumentError(
                self, 'Each option should be in the form "key=value" (e.g. format=MSEED')
        setattr(namespace, 'o', d)


class procNum(argparse.Action):
    '''A argparse action subclass.
       Called if options parameters are entered at command-line
    '''
    def __call__(self, parser, namespace, value, option_string=None):
        if value > MAXProcs:
            raise argparse.ArgumentError(
                self, "Can't use more than the system's processors number (max - %d)" % MAXProcs)
        setattr(namespace, 'n', value)


# set the argument parser
parser = argparse.ArgumentParser(
    formatter_class=argparse.RawDescriptionHelpFormatter,
    description='''Seedlink Reat-Time Playback''',
    epilog=textwrap.dedent('''\
        The code will read seismic waveforms and send it to a seedlink server
        over datalink protocol, simulating a real time behavior, by adjusting
        waveform time to current time.

        Created by Ran Novitsky Nof (ran.nof@gmail.com) @ BSL, 2015'''))

parser.add_argument('p', metavar='PATH', nargs='+',
                    help='Path of data (use quotes for "PATH" if using regex)')
parser.add_argument('-t', metavar='Start', nargs='+', type=UTCDateTime,
                    help='Start and end times. See obspy.UTCDateTime for acceptable string formats. Usually YYYYMMDDhhmmss.f.\nDefault - None',
                    default=[None, None], action=getStartEndTimes)
parser.add_argument('-ti', metavar='hh:mm:ss', type=getTimeOfDay,
                    help='Initial time of day to start the replay (hh:mm:ss or hh:mm:ss.f). Data before this time will not be sent.\nDefault - None',
                    default=None)
parser.add_argument('-tf', metavar='hh:mm:ss', type=getTimeOfDay,
                    help='Final time of day to stop the replay (hh:mm:ss or hh:mm:ss.f). Data after this time will not be sent.\nDefault - None',
                    default=None)
parser.add_argument('-o', metavar='options', nargs='+',
                    help='additional optional parameters. The C engine reads MiniSEED directly; only format=MSEED is validated.',
                    default={}, action=getReadOptions)
parser.add_argument('-s', metavar='seconds', type=float,
                    help='Packet time interval in seconds. Default - 1 sec.',
                    default=1.0)
parser.add_argument('-l', metavar='Latency', type=getLatency,
                    help='Latency of data. Can be one argument for all stations in seconds or a file name \
containing a list of net.sta=X format. Default - 0.',
                    default=0)
parser.add_argument('-m', action='store_true',
                    help='Modify time to current time (relative to playback start time)',
                    default=False)
parser.add_argument('-f', action='store_true',
                    help='Fast replay - No delay (Not a "real time" playback)',
                    default=False)
parser.add_argument('-H', metavar='Host', help='Host Address (127.0.0.1)',
                    default="127.0.0.1")
parser.add_argument('-P', metavar='Port', help='Port Number (16000)',
                    default="16000")
parser.add_argument('-O', metavar='file', help='send output to file',
                    default=None)
parser.add_argument('-v', action='store_true', help='Verbose (False).',
                    default=False)
parser.add_argument('-d', metavar='delay', type=float,
                    help='Directly stream ordered file to server with delay ( in seconds) every 250 packets.',
                    default=-1)
parser.add_argument('-n', metavar='processors', type=int,
                    help='Accepted for compatibility; the C engine is single-process',
                    default=1, action=procNum)
parser.add_argument('--test', action='store_true',
                    help='Testing mode - no data will be sent. (False)',
                    default=False)


def findEngine():
    '''Locate the srtpbc executable next to this script or in src/.'''
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(here, 'srtpbc'),
        os.path.join(here, '..', 'src', 'srtpbc'),
    ]
    for candidate in candidates:
        candidate = os.path.abspath(candidate)
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    found = shutil.which('srtpbc')
    if found:
        return found
    raise SystemExit(
        "Cannot find the srtpbc engine.\n"
        "Build it with 'make' from the repository root (or 'make -C src').")


def to_us(t):
    '''Convert an ObsPy UTCDateTime to integer microseconds since epoch.'''
    if t is None:
        return None
    try:
        return int(t.ns // 1000)
    except AttributeError:
        return int(round(float(t.timestamp) * 1000000.0))


def to_sod(t):
    '''Convert a (hh, mm, ss) tuple to seconds since 00:00:00.'''
    if t is None:
        return None
    hh, mm, ss = t
    return hh * 3600.0 + mm * 60.0 + ss


def expandFiles(patterns):
    '''Expand glob patterns the same way obspy.read does for a single path.'''
    files = []
    for pattern in patterns:
        matches = sorted(glob.glob(pattern))
        if matches:
            files.extend(matches)
        else:
            files.append(pattern)
    return files


def checkReadOptions(options):
    '''The C engine reads MiniSEED directly; only format is meaningful.'''
    unsupported = []
    for key, value in options.items():
        if key == 'format':
            value = str(value).strip().strip("'\"")
            if value.upper() not in ('MSEED', 'MINISEED'):
                raise SystemExit(
                    "The C engine only reads MiniSEED data (got -o format=%s)." % value)
        else:
            unsupported.append(key)
    if unsupported:
        print("Warning: the C engine reads MiniSEED directly and ignores -o %s"
              % ', '.join(unsupported), file=sys.stderr)


def buildCommand(args):
    engine = findEngine()
    cmd = [engine,
           '--host', str(args.H),
           '--port', str(args.P),
           '--dt', repr(float(args.s))]

    if isinstance(args.l, dict):
        cmd += ['--latency-default', '0']
        for key in sorted(args.l):
            cmd += ['--latency', '%s=%s' % (key, repr(float(args.l[key])))]
    else:
        cmd += ['--latency-default', repr(float(args.l))]

    if args.m:
        cmd.append('--modify')
    if args.f:
        cmd.append('--fast')
    if args.test:
        cmd.append('--test')
    if args.v:
        cmd.append('--verbose')
    if args.O:
        cmd += ['--output', str(args.O)]
    if args.d >= 0:
        cmd += ['--direct', repr(float(args.d))]

    if args.t and args.t[0] is not None:
        cmd += ['--start-us', str(to_us(args.t[0]))]
    if args.t and args.t[1] is not None:
        cmd += ['--end-us', str(to_us(args.t[1]))]
    if args.ti is not None:
        cmd += ['--ti', repr(to_sod(args.ti))]
    if args.tf is not None:
        cmd += ['--tf', repr(to_sod(args.tf))]

    cmd.append('--')
    cmd.extend(expandFiles(args.p))
    return cmd


def main(args):
    startRun = UTCDateTime.now()
    info = sys.stderr if args.O == '-' else sys.stdout

    print("Start: %s" % startRun, file=info)

    if args.n != 1:
        print("Note: -n is ignored by the C engine (single process).",
              file=sys.stderr)
    checkReadOptions(args.o)

    cmd = buildCommand(args)
    rc = subprocess.call(cmd)

    endRun = UTCDateTime.now()
    print("End: %s (%lf)" % (endRun, endRun - startRun), file=info)
    return rc


if __name__ == '__main__':
    args = parser.parse_args(sys.argv[1:])
    sys.exit(main(args))
