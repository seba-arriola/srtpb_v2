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

# srtpblib is a C library wrapped by swig for python use. must be compiled before use.
from srtpblib import srtpb_record_handler_slink as daliwrite
from srtpblib import *
from obspy.io.mseed.core import _write_mseed as writeMSEED
from obspy import read,UTCDateTime,Stream
from io import BytesIO
class BytesIOWrapper(BytesIO):
    @property
    def len(self):
        pos = self.tell()
        self.seek(0, 2)
        l = self.tell()
        self.seek(pos)
        return l
    @property
    def buf(self):
        return self.getvalue()
StringIO = BytesIOWrapper
xrange = range
import numpy as np
import threading
from threading import Timer
import multiprocessing as mp
from multiprocessing import Pool,Manager,Queue,Lock,Process
import argparse,os,sys,textwrap,time

_verbose=False
_test=False
MAXTHREADS=300 # should be higher than the number of station.channels so at each point of time all data from all stations will be sent.
MAXProcs = mp.cpu_count()-1 # for multiprocessing when slicing a Stream
if not MAXProcs: MAXProcs = 1 # make sure at least one processor is available
Nwriters = 1
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
  except:
    msg = "Time of day must be in hh:mm:ss or hh:mm:ss.f format (e.g. 22:49:17)"
    raise argparse.ArgumentTypeError(msg)

def getLatency(s):
  '''a argparse function for latency commandline variable
  checking for a number of a file name, and return a number
  or a dictionary based on the file data.
  file should hold a list of NET.STA=latency for stations.
  stations on on the list will have 0 latency.
  '''
  try: # check if its a number like parameter
    l = float(s)
    return l
  except:
    pass
  if not os.path.exists(s): # check if its a file
    msg = "Can't find latency file %r" % s
    raise argparse.ArgumentTypeError(msg)
  try:
    f = open(s,'r')
    lines = [l.strip().split('=') for l in f.readlines()] #reads the lines,split on '=' and create a dictionary
    f.close()
    l = dict([(id,float(latency)) for id,latency in lines])
    return l
  except:
    msg = "File Format Error in %r." % s
    raise argparse.ArgumentTypeError(msg)

class getStartEndTimes(argparse.Action):
  '''A argparse action subclass.
     Called if a start-end time parameters are entered at command-line
  '''
  def __call__(self, parser, namespace, values, option_string=None):
    if len(values)>2: #make sure only two parameters are given
      raise argparse.ArgumentError(self,'Need one or two parameters.')
    start = values[0] # get start time
    if len(values)==2:
      end = values[1] # get end time
    else:
      end=None
    setattr(namespace, 't', [start,end]) # assign them to parameter t

class getReadOptions(argparse.Action):
  '''A argparse action subclass.
     Called if options parameters are entered at command-line
  '''
  def __call__(self, parser, namespace, values, option_string=None):
    try:
      d = dict([v.split('=') for v in values]) # create a dictionary from options
    except ValueError:
      raise argparse.ArgumentError(self,'Each option should be in the form "key=value" (e.g. format=MSEED')
    setattr(namespace, 'o', d) # assign dictionary to parameter o

class procNum(argparse.Action):
  '''A argparse action subclass.
     Called if options parameters are entered at command-line
  '''
  def __call__(self, parser, namespace, value, option_string=None):
    if value>MAXProcs:
      raise argparse.ArgumentError(self,"Can't use more than the system's processors number (max - %d)"% MAXProcs)
    setattr(namespace, 'n', value) # assign dictionary to parameter o

# set the argument parser
parser = argparse.ArgumentParser(
         formatter_class=argparse.RawDescriptionHelpFormatter,
         description='''Seedlink Reat-Time Playback''',
         epilog=textwrap.dedent('''\
             The code will read seismic waveforms and send it to a seedlink server
             over datalink protocol, simulating a real time behavior, by adjusting
             waveform time to current time.

             Created by Ran Novitsky Nof (ran.nof@gmail.com) @ BSL, 2015'''))

parser.add_argument('p',metavar='PATH',nargs='+',help='Path of data (use quotes for "PATH" if using regex)')
parser.add_argument('-t',metavar='Start',nargs='+',type=UTCDateTime,help='Start and end times. See obspy.UTCDateTime for acceptable string formats. Usually YYYYMMDDhhmmss.f.\nDefault - None',default=[None,None],action=getStartEndTimes)
parser.add_argument('-ti',metavar='hh:mm:ss',type=getTimeOfDay,help='Initial time of day to start the replay (hh:mm:ss or hh:mm:ss.f). Data before this time will not be sent.\nDefault - None',default=None)
parser.add_argument('-tf',metavar='hh:mm:ss',type=getTimeOfDay,help='Final time of day to stop the replay (hh:mm:ss or hh:mm:ss.f). Data after this time will not be sent.\nDefault - None',default=None)
parser.add_argument('-o',metavar='options',nargs='+',help='additional optional parameters for read function. See obspy.read for help',default={},action=getReadOptions)
parser.add_argument('-s',metavar='seconds',type=float,help='Packet time interval in seconds. Default - 1 sec.',default=1.0)
parser.add_argument('-l',metavar='Latency',type=getLatency,help='Latency of data. Can be one argument for all stations in seconds or a file name \
containing a list of net.sta=X format. Default - 0.',default=0)
parser.add_argument('-m',action='store_true',help='Modify time to current time (relative to playback start time)',default=False)
parser.add_argument('-f',action='store_true',help='Fast replay - No delay (Not a "real time" playback)',default=False)
parser.add_argument('-H',metavar='Host',help='Host Address (127.0.0.1)',default="127.0.0.1")
parser.add_argument('-P',metavar='Port',help='Port Number (16000)',default="16000")
parser.add_argument('-O',metavar='file',help='send output to file',default=None)
parser.add_argument('-v',action='store_true',help='Verbose (False).',default=False)
parser.add_argument('-d',metavar='delay',type=float,help='Directly stream ordered file to server with delay ( in seconds) every 250 packets.',default=-1)
parser.add_argument('-n',metavar='processors',type=int,help='Number of processors for fast replay',default=1,action=procNum)
parser.add_argument('--test',action='store_true',help='Testing mode - no data will be sent. (False)',default=False)


def slinkOpen(IP="127.0.0.1:16000",v=4):
  'open seedlink data connection. Uses srtpblib c-python library'
  if not srtpbdaliinit(IP,"SRTPB",v): sys.exit('Cannot connect to server [%s].\nExit.'%IP)

def slinkClose():
  'close seedlink data connection. Uses srtpblib c-python library'
  srtpbdaliquit()


def getData(filelist,starttime,endtime,**kwargs):
  '''
  reads the data from the filelist and extract data between start and end.
  kwargs can be any parameter of obspy read function.
  '''
  S = Stream()
  for f in filelist:
    try:
      S += read(f,starttime=starttime,endtime=endtime,**kwargs)
    except Exception as msg:
      print(msg.args[0], file=sys.stderr)
  stations = list(set([t.stats.network+'.'+t.stats.station for t in S])) # get a list of stations in NET.STA format
  if len(S)<1:
    raise ValueError("No data could be found.")
  return stations,S

def setLatency(S,latency):
  '''Sets the latency for each trace in stream S
     according to the latency.
     latency can be similar to all stations if latency s float
     or separately assigned according to a dictionary of the form {NET.STA:latency}.
  '''
  if type(latency)==dict:
    [t.stats.__setattr__('latency',latency[t.stats.network+'.'+t.stats.station]) for t in S if t.stats.network+'.'+t.stats.station in latency]
  elif isinstance(latency,(int,float)):
    [t.stats.__setattr__('latency',latency) for t in S]
  else:
    return

def getSlice(lst,Qin,Qout,L):
  run = True
  while run:
    try:
      starttime,PacketTimeInterval,i,m,T0,Tpb = Qin.get()
    except:
      run=False
      continue
    s=lst[0].slice(starttime+i*PacketTimeInterval,starttime+(i+1)*PacketTimeInterval)
    for t in s:
      t.stats.starttime = t.stats.starttime-t.stats.latency # shift back traces times according to latencies
      if len(t.data)>1: t.data = t.data[:-1] # avoids overlaps of 1 sample. However, the last sample of a trace in the dataset will be lost.
      if m:
        dt = t.stats.starttime-T0 # time since start of data
        t.stats.starttime = Tpb+dt # assign starttime relative to time of playback
    Qout.put(s)

def putSlice(Qin,L,):
  slinkOpen(':'.join([args.H,args.P]),args.v) # open seedlink server for data connection
  run = True
  S = Stream()
  while run:
    s = Qin.get()
    if s=='done.':
      run=False
      continue
    S +=s
  d = len(S)
  S.sort(['starttime'])
  L.acquire()
  if _verbose: print('Writting %d records'%(d), file=sys.stderr)
  L.release()
  buff = StringIO()
  writeMSEED(S, buff,11,512) # write 512byte packets of mseed at STEIM2 compression (from obspy.mseed.core) to buffer
  buff.seek(0) # rewinde buffer
  tic = UTCDateTime.now()
  L.acquire()
  if _verbose: print('Sending %d packets @ %s'%(int(buff.len/512),tic), file=sys.stderr)
  L.release()  
  if not _test:
    for i in xrange(int(buff.len/512)):
      if i%250==0 and not args.O: time.sleep(0.1) # must slow down for ElarmS fast playback mode
      daliwrite(buff.buf[i*512:(i+1)*512],512,0,1)
  toc = UTCDateTime.now()
  L.acquire()
  if _verbose: print('Sending ended @ %s (%lf)'%(toc,toc-tic), file=sys.stderr)
  L.release()
  slinkClose()


def sendStreamFast(S,PacketTimeInterval=1.0,latency=0,T0=None,Tpb=None,m=False):
  """Send a stream to the server.
  """
  tic = UTCDateTime.utcnow()
  if _verbose: print('Sorting started at: %s'%tic, file=sys.stderr)
  if _verbose: print('Sorting data in %d original packets...'%len(S), file=sys.stderr)
  S.merge()
  S = S.split()
  [t.stats.__setattr__('latency',latency) for t in S if not 'latency' in t.stats]
  
  starttime = S.sort(['starttime'])[0].stats.starttime
  endtime = S.sort(['endtime'])[-1].stats.endtime

  [t.stats.__setattr__('starttime',t.stats.starttime+t.stats.latency) for t in S]
  for t in S:
    t.data=t.data.astype(np.int32)
    
  elatency = S[-1].stats.latency if len(S)>0 else 0
  slatency = S[0].stats.latency if len(S)>0 else 0
  NTI = int(np.ceil((endtime-starttime)/float(PacketTimeInterval)))
  if _verbose: print('Slicing %d Traces into ~%d packets...'%(len(S),NTI*len(list(set([t.id for t in S])))), file=sys.stderr)
  if not T0: T0 = starttime-slatency
  if not Tpb: Tpb = UTCDateTime.utcnow()
  lst.append(S)
  tic = UTCDateTime.utcnow()
  [Qin.put([starttime,PacketTimeInterval,i,m,T0,Tpb]) for i in xrange(NTI)]
  [Qin.put(['Done.']) for i in xrange(args.n)]
  [p.join() for p in SlicerPool]
  toc = UTCDateTime.utcnow()
  if _verbose: print('Data sent to slicing at: %s (%lf)'%(toc,toc-tic), file=sys.stderr)
  [Qout.put('done.') for i in xrange(Nwriters)]
  [writer.join() for writer in writers]
  toc = UTCDateTime.utcnow()
  if _verbose: print('Data sent to slink server at: %s (%lf)'%(toc,toc-tic), file=sys.stderr)
  return [],Tpb,starttime,endtime

def sendStream(S,PacketTimeInterval=1.0,latency=0,T0=None,Tpb=None,m=False):
  """Send a stream to the server using natural global timeline and time-ordered packet interleaving.
     Extracts 512-byte packets per trace per time window and interleaves them so all channels
     and stations active in the window are sent together.
  """
  [t.stats.__setattr__('latency',latency) for t in S if not 'latency' in t.stats]
  if len(S) == 0:
    return [], Tpb, UTCDateTime.utcnow(), UTCDateTime.utcnow()
    
  starttime = S.sort(['starttime'])[0].stats.starttime
  endtime = S.sort(['endtime'])[-1].stats.endtime
  
  if not T0: T0 = starttime
  if not Tpb: Tpb = UTCDateTime.utcnow()
  
  total_duration = endtime - starttime
  NTI = int(np.ceil(total_duration / float(PacketTimeInterval)))
  
  if _verbose: print('Sending (%s) with packet-interleaved loop: NTI=%d intervals (dt=%.3fs)'%(Tpb, NTI, PacketTimeInterval), file=sys.stderr)

  def worker():
    for i in xrange(NTI):
      t_start = starttime + i * PacketTimeInterval
      t_end = starttime + (i + 1) * PacketTimeInterval
      
      s = S.slice(t_start, t_end).copy()
      if len(s) == 0:
        continue
      
      target_time = Tpb + (i * PacketTimeInterval) + PacketTimeInterval
      sleep_duration = target_time - UTCDateTime.utcnow()
      if sleep_duration > 0 and not _test:
        time.sleep(sleep_duration)
      
      if not _test:
        s.sort(['starttime', 'channel'])
        for t in s:
          t.stats.starttime = t.stats.starttime - t.stats.latency
          dt = t.stats.starttime - T0
          if m: t.stats.starttime = Tpb + dt
          if len(t.data) > 1: t.data = t.data[:-1]
          t.data = t.data.astype(np.int32)
        
        all_packets = []
        for t in s:
          st_single = Stream([t])
          buff = StringIO()
          writeMSEED(st_single, buff, 11, 512)
          buff.seek(0)
          buf_data = buff.getvalue()
          rec_start = int(t.stats.starttime.timestamp * 1e6)
          rec_end = int(t.stats.endtime.timestamp * 1e6)
          num_pkts = len(buf_data) // 512
          for j in range(num_pkts):
            chunk = buf_data[j*512:(j+1)*512]
            if len(chunk) == 512:
              all_packets.append((chunk, rec_start, rec_end))
              
        for chunk, rec_start, rec_end in all_packets:
          daliwrite(chunk, 512, rec_start, rec_end)
              
  if not _test:
    t_worker = threading.Thread(target=worker)
    t_worker.start()
    return [t_worker], Tpb, starttime, endtime
  else:
    return [], Tpb, starttime, endtime


def resolveTimeWindow(filelist,starttime,endtime,ti,tf):
  '''Combine the -ti/-tf (time of day) parameters with the data date to produce
     a full starttime/endtime cut for the replay. Returns (starttime,endtime).
     ti, tf - (hh,mm,ss) tuples or None.
  '''
  if ti is None and tf is None:
    return starttime, endtime
  # find the reference date: use -t start date if given, otherwise the earliest trace start
  ref = starttime
  if ref is None:
    for f in filelist:
      try:
        h = read(f, headonly=True)
      except Exception as msg:
        print(msg.args[0], file=sys.stderr)
        continue
      if len(h) > 0:
        cand = min([t.stats.starttime for t in h])
        if ref is None or cand < ref:
          ref = cand
  if ref is None:
    raise ValueError("Cannot determine a date to interpret -ti/-tf.")
  if ti is not None:
    starttime = UTCDateTime(ref.year, ref.month, ref.day, ti[0], ti[1], ti[2])
  if tf is not None:
    endtime = UTCDateTime(ref.year, ref.month, ref.day, tf[0], tf[1], tf[2])
  return starttime, endtime


def main(args):
  filelist = args.p # get the file list
  starttime,endtime = args.t # get time interval
  starttime,endtime = resolveTimeWindow(filelist,starttime,endtime,args.ti,args.tf) # apply -ti/-tf cuts
  stations,S = getData(filelist,starttime,endtime,**args.o) # read the data
  setLatency(S,args.l) # set latencies
  if args.f:
    if _verbose: print('Running in Fast mode...', file=sys.stderr)
    timers,Tpb,starttime0,endtime0 = sendStreamFast(S,args.s,m=args.m) # send the stream as fast as possible
  else:
    slinkOpen(':'.join([args.H,args.P]),args.v) # open seedlink server for data connection
    timers,Tpb,starttime0,endtime0 = sendStream(S,args.s,m=args.m) # send the stream
  if _verbose: print('Waiting for data to be sent...', file=sys.stderr)
  if not args.test: [t.join() for t in timers] # wait for threads to finish
  if not args.f: slinkClose() # close connection
  print("Playback started at: %s\nData span: %s - %s\nDelta (sec): %s"%(Tpb,starttime0,endtime0,Tpb-starttime0))


def direct(args):
  assert args.t==[None,None],'Error: Direct Mode cannot use time value.'
  slinkOpen(':'.join([args.H,args.P]),args.v) # open seedlink server for data connection
  filelist = args.p # get the file list
  for file in filelist:
    tic = UTCDateTime.now()
    try:
      buffer = open(file,'rb').read()
    except Exception as E:
      print('Error with file %s (%s)'%(file,str(E)), file=sys.stderr)
      continue
    for i in xrange(int(len(buffer)/512)):
      if i%250==0: time.sleep(args.d)
      daliwrite(buffer[i*512:(i+1)*512],512,0,1)
    if _verbose: print('%s sent in %lf sec'%(file,UTCDateTime.now()-tic), file=sys.stderr)

def filewrite(buff,bufflen,start,end):
  outputFileHandler.write(buff)

def dummy(*args,**kwargs):
  pass  

if __name__=='__main__':
  startRun = UTCDateTime.now()
  print("Start: %s"%startRun)
    # parse the arguments
  args = parser.parse_args(sys.argv[1:])
  if args.v:
    _verbose=True
    args.v=4
  else:
    _verbose=0
  if args.test: _test=True
  if args.O:
    if args.O == '-': 
      outputFileHandler = sys.stdout
    else:
      outputFileHandler = open(args.O,'ab')
    daliwrite = filewrite
    slinkOpen = dummy
    slinkClose = dummy
  if args.f:
    # multiprocessing stuff
    if _verbose: print('Staring %d subprocesses for fast slicing.'%args.n, file=sys.stderr)
    Qin = Queue(int(args.n*1.2+1))
    Qout = Queue(int(args.n*1.2+1))
    L = Lock()
    lst = Manager().list()
    SlicerPool = [Process(target=getSlice,args=(lst,Qin,Qout,L)) for i in xrange(args.n)]
    [p.start() for p in SlicerPool]
    writers = [Process(target=putSlice,args=(Qout,L)) for i in xrange(Nwriters)]
    [writer.start() for writer in writers]
  if not args.d>=0:
    main(args)
  else:
    direct(args)
  endRun = UTCDateTime.now()
  print("End: %s (%lf)"%(endRun,endRun-startRun))

