#!/usr/bin/env python3
"""Real local processes/sockets and existing generated manifests. No NAT claim.
All generated evidence, process logs and caches live in a temporary directory.
Ports 37401/37402/37405 must be free. Tests use the current one-room Tracker.
"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]
BASE=Path(tempfile.mkdtemp(prefix='fragment-transfer-tests-'))

def text(path):
    return path.read_text(errors='replace') if path.exists() else ''

def wait_for(predicate, label, timeout=12):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        if predicate(): return
        time.sleep(.04)
    raise AssertionError('timeout: '+label+'; inspect logs under '+str(BASE))

class Run:
    def __init__(self,name):
        self.dir=BASE/name; self.dir.mkdir(); (self.dir/'logs').mkdir();self.ps={};self.handles=[]
        subprocess.run(['python3',str(ROOT/'tools/gen_case.py')],cwd=self.dir,check=True,capture_output=True)
        self.session=next(self.dir.glob('session_*'))
        self.sid=int(self.session.name[8:],16)
        self.start('room',[str(ROOT/'room_server')]);
        # Don't open a throwaway TCP connection: check actual startup log instead.
        wait_for(lambda:'UDP 监听' in self.log('room'),'room ready')
    def start(self,name,cmd,env=None):
        f=open(self.dir/(name+'.log'),'w');self.handles.append(f)
        p=subprocess.Popen(cmd,cwd=self.dir,stdin=subprocess.PIPE,stdout=f,stderr=subprocess.STDOUT,text=True,env=env)
        self.ps[name]=p;return p
    def log(self,name):return text(self.dir/(name+'.log'))
    def client(self,name,host=False,auto=False,mode='tcp',delay=0,cache=None,drop=None):
        cmd=[str(ROOT/'client_transfer'),'--room','127.0.0.1','--nick',name,'--run-seconds','90','--mode',mode,'--delay-ms',str(delay)]
        if host:cmd+=['--host','--session',str(self.session)]
        else:cmd+=['--data-dir',str(cache or self.dir/name)]
        if auto:cmd+=['--auto']
        env=os.environ.copy()
        if drop:
            assert (ROOT/'tests/udp_fault.so').is_file(), 'Run make test-transfer to build fault injector'
            key = ('DYLD_INSERT_LIBRARIES'
                   if __import__('sys').platform == 'darwin'
                   else 'LD_PRELOAD')
            env[key] = str(ROOT/'tests/udp_fault.so')
            env['FA_TEST_UDP_DROP'] = drop
        self.start(name,cmd,env)
        wait_for(lambda:'JOINED' in self.log(name),name+' joined')
    def command(self,name,line):
        self.ps[name].stdin.write(line+'\n');self.ps[name].stdin.flush()
    def complete(self,name,cache=None,timeout=15):
        wait_for(lambda:all(f'TRANSFER FILE_COMPLETE {f} ' in self.log(name) for f in (1,2,3)),name+' files',timeout)
        dest=(cache or self.dir/name)/self.session.name
        for f in (1,2,3):assert (dest/f'file_{f}').read_bytes()==(self.session/f'file_{f}').read_bytes()
    def stop(self,name):
        p=self.ps[name]
        if p.poll() is None:p.terminate();p.wait(timeout=3)
    def close(self):
        for p in self.ps.values():
            if p.poll() is None:p.terminate()
        for p in self.ps.values():p.wait(timeout=3)
        for f in self.handles:f.close()

def tcp_faults():
    r=Run('tcp');
    try:
        r.client('Host',host=True);r.client('Bob',auto=True);r.complete('Bob')
        r.client('Carol',delay=350)
        wait_for(lambda:'MANIFEST_READY' in r.log('Carol'),'Carol manifest')
        time.sleep(.3)
        assert 'FILE_COMPLETE' not in r.log('Carol'), 'remote BITFIELD must not unlock local evidence'
        # Download after Tracker loss, then remove a source during transfer.
        r.stop('room');r.command('Carol','get')
        wait_for(lambda:'TRANSFER PIECE_OK 2 ' in r.log('Carol'),'B serves C')
        r.stop('Bob');r.complete('Carol')
        assert 'TRANSFER PIECE_OK 1 ' in r.log('Carol')
        assert 'TRACKER_OFFLINE' in r.log('Carol')
        r.command('Carol','stats');time.sleep(.1)
        print('PASS TCP: verified files, B->C upload, multisource, Tracker loss, source loss, no false HAVE')
    finally:r.close()

def resume():
    r=Run('resume')
    try:
        r.client('Host',host=True);r.client('Bob',auto=True,delay=400)
        wait_for(lambda:r.log('Bob').count('TRANSFER PIECE_OK')>=2,'partial download')
        r.stop('Bob');time.sleep(.2)
        partial=r.dir/'Bob'/r.session.name/'file_1';assert partial.exists()
        # Add damaged bytes for one already present block: startup must revalidate it.
        content=bytearray(partial.read_bytes());content[0]^=1;partial.write_bytes(content)
        r.client('Bob',auto=True);r.complete('Bob')
        assert 'TRANSFER RESUME_BLOCK 1 0' not in r.log('Bob')
        assert 'TRANSFER RESUME_BLOCK 1 1' in r.log('Bob')
        print('PASS restart: reused persisted cache, rejected damaged cached block, same nickname rejoin')
    finally:r.close()

def udp(mode):
    r=Run('udp_'+mode)
    try:
        if mode=='all':
            r.start('relay',[str(ROOT/'relay_server')]);wait_for(lambda:('RELAY_READY' in r.log('relay') or '监听 0.0.0.0:37405' in r.log('relay')),'relay ready')
        r.client('Host',host=True,mode='udp',drop=mode)
        r.client('Bob',auto=True,mode='udp')
        ports=[int(re.search(r'JOINED .*udp=(\d+)',r.log(n))[1]) for n in ('Host','Bob')]
        assert ports[0]!=ports[1], 'UDP sockets share a port; rebuild client_transfer with FA_TRANSFER_CLIENT'
        wait_for(lambda:'UDP_READY' in r.log('Bob'),'UDP peer path ready',6)
        wait_for(lambda:'FAULT_DROP' in r.log('Host'),'fault injector dropped real PIECE_DATA',6)
        r.complete('Bob',timeout=25)
        assert 'FAULT_DROP' in r.log('Host'), 'UDP fault injection did not activate'
        assert 'UDP_RETRY' in r.log('Bob')
        if mode=='all':
            assert 'UDP_FALLBACK' in r.log('Bob') and 'RELAY_FORWARD' in r.log('relay')
            r.command('Host','stats');time.sleep(.1)
            assert re.search(r'relay_up=[1-9]',r.log('Host'))
            print('PASS UDP total loss: bounded retries -> actual TCP relay -> verified files')
        else:
            assert 'UDP_FALLBACK' not in r.log('Bob')
            print('PASS UDP selective loss: NAK missing subpacket recovery and verified short final blocks')
    finally:r.close()

def relay_checks():
    r=Run('relay')
    sockets=[]
    def frame(cmd,body,req=1):return struct.pack('!HBBHH',0x4641,1,cmd,req,len(body))+body
    def recv_frame(s):
        def exact(n):
            b=b''
            while len(b)<n:
                v=s.recv(n-len(b));assert v;b+=v
            return b
        h=exact(8);_,_,cmd,req,n=struct.unpack('!HBBHH',h);return cmd,exact(n)
    try:
        r.client('Host',host=True,mode='relay');r.client('Bob',mode='relay')
        r.start('relay',[str(ROOT/'relay_server')]);wait_for(lambda:('RELAY_READY' in r.log('relay') or '监听 0.0.0.0:37405' in r.log('relay')),'relay ready')
        r.command('Bob','get');r.complete('Bob',timeout=15)
        # A newly-bound socket replaces peer 2. Malformed spoofed source is rejected.
        s=socket.create_connection(('127.0.0.1',37405),timeout=2);sockets.append(s)
        inner=frame(4,b'',0)
        bind=frame(0x10,struct.pack('!BH',2,1));wrap=frame(0x11,struct.pack('!BBH',1,3,len(inner))+inner,0)
        for b in bind[:7]:s.sendall(bytes([b]))
        s.sendall(bind[7:]+wrap)
        cmd,body=recv_frame(s);assert cmd==0x13 and body==bytes.fromhex('0012');assert s.recv(1)==b''
        r.client('Carol',mode='relay')
        # Freeze the membership snapshot, then use raw sockets without clients reconnecting.
        r.stop('room')
        for name in ('Host','Bob','Carol'): r.stop(name)
        def connect_peer(peer,room=1,body=None,small_buffer=False):
            sock=socket.socket();sock.settimeout(3)
            if small_buffer:sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,1024)
            sock.connect(('127.0.0.1',37405));sockets.append(sock)
            sock.sendall(frame(0x10,body if body is not None else struct.pack('!BH',peer,room)))
            return sock
        for peer,room,body,code in ((1,2,None,'0012'),(4,1,None,'0012'),
                                    (1,1,b'\x01','0004')):
            sock=connect_peer(peer,room,body)
            cmd,b=recv_frame(sock)
            assert cmd==0x13 and b==bytes.fromhex(code),(cmd,b)
            assert sock.recv(1)==b''
        # One complete 16 KiB data block, coalesced with BIND after a split header.
        dest=connect_peer(1)
        wait_for(lambda:r.log('relay').count('RELAY_BOUND peer=1')>=2,'raw destination bind')
        src=socket.create_connection(('127.0.0.1',37405),timeout=3);sockets.append(src)
        data=bytes(range(256))*64
        payload=struct.pack('!IBBHHHH',r.sid,1,0,0,0,1,len(data))+data
        inner=frame(0x0b,payload,77)
        wrapped=struct.pack('!BBH',1,2,len(inner))+inner
        bind=frame(0x10,struct.pack('!BH',2,1))
        src.sendall(bind[:3]);time.sleep(.02)
        src.sendall(bind[3:]+frame(0x11,wrapped,0))
        assert recv_frame(dest)==(0x11,wrapped)
        # Multiple complete frames in a single write must remain separate.
        src.sendall(frame(0x11,wrapped,0)*2)
        assert recv_frame(dest)==(0x11,wrapped)
        assert recv_frame(dest)==(0x11,wrapped)
        # A peer which never reads must not block unrelated forwarding.
        slow=connect_peer(3,small_buffer=True)
        wait_for(lambda:r.log('relay').count('RELAY_BOUND peer=3')>=1,'slow bind')
        to_slow=struct.pack('!BBH',3,2,len(inner))+inner
        for _ in range(512):src.sendall(frame(0x11,to_slow,0))
        src.sendall(frame(0x11,wrapped,0))
        assert recv_frame(dest)==(0x11,wrapped)
        # Malformed outer length closes only the offending connection.
        bad=connect_peer(2)
        bad.sendall(frame(0x11,struct.pack('!BBH',1,2,9)+b'12345678',0))
        assert recv_frame(bad)==(0x13,bytes.fromhex('0010'))
        assert bad.recv(1)==b''
        assert r.ps['relay'].poll() is None
        print('PASS relay: 16KiB, split/coalesced frames, room/membership/source/length checks, slow-reader isolation')
    finally:
        for s in sockets:s.close()
        r.close()

def announce_listener():
    # Directed local ANNOUNCE proves demultiplexing only, not LAN broadcast or peer-0 downloading.
    d=BASE/'announce';d.mkdir();(d/'logs').mkdir()
    with open(d/'listener.log','w') as log:
        p=subprocess.Popen([str(ROOT/'client_transfer'),'--run-seconds','10'],cwd=d,
                           stdin=subprocess.PIPE,stdout=log,stderr=subprocess.STDOUT,text=True)
        sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        try:
            body=struct.pack('!HI4sHHB',1,0x12345678,socket.inet_aton('127.0.0.1'),1,40000,4)+b'Test'
            packet=struct.pack('!HBBHH',0x4641,1,0x12,0,len(body))+body
            def received():
                sock.sendto(packet,('127.0.0.1',37404))
                return 'ANNOUNCE_RECEIVED room=1' in text(d/'listener.log')
            wait_for(received,'separate ANNOUNCE receiver',3)
            assert 'ANNOUNCE listener unavailable' not in text(d/'listener.log')
            print('PASS ANNOUNCE listener: port 37404 retained alongside independent transfer UDP socket')
        finally:
            sock.close()
            if p.poll() is None:p.terminate()
            p.wait(timeout=3)

def invalid_manifests():
    d=BASE/'invalid';d.mkdir()
    subprocess.run(['python3',str(ROOT/'tools/gen_case.py')],cwd=d,check=True,capture_output=True)
    session=next(d.glob('session_*'));path=session/'manifest.bin';original=path.read_bytes()
    cases=[original[:20],original+b'x']
    for offset,value in ((36,4),(37,0),(38,2)):
        bad=bytearray(original);bad[offset]=value;cases.append(bytes(bad))
    bad=bytearray(original);bad[40:44]=struct.pack('!I',512*1024+1);cases.append(bytes(bad))
    for data in cases:
        path.write_bytes(data)
        result=subprocess.run([str(ROOT/'client_transfer'),'--host','--session',str(session)],
                              cwd=d,input='',text=True,capture_output=True,timeout=3)
        assert result.returncode==1 and 'Invalid session/manifest' in result.stderr
    print('PASS manifest: truncation, trailing bytes, file count/id/chapter, oversize rejected')

import argparse
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--case', choices=['all','manifest','tcp','resume','udp-once','udp-all','relay','announce'], default='all')
args=parser.parse_args()
cases={'manifest':invalid_manifests,'tcp':tcp_faults,'resume':resume,
       'udp-once':lambda:udp('once'),'udp-all':lambda:udp('all'),'relay':relay_checks,'announce':announce_listener}
import json,platform,sys
metadata={'revision':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
          'working_tree':subprocess.check_output(['git','status','--short'],cwd=ROOT,text=True),
          'platform':platform.platform(),'python':sys.version,'case':args.case,'relay':'relay_server'}
(BASE/'environment.json').write_text(json.dumps(metadata,indent=2))
try:
    for name,run in cases.items():
        if args.case in ('all',name): run()
    print('ALL TRANSFER INTEGRATION TESTS PASS' if args.case=='all' else 'PASS selected case: '+args.case)
finally:
    print('Evidence directory:',BASE)
