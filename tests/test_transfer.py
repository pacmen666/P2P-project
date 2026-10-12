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
    raise AssertionError('timeout: '+label)

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
        if drop:env.update(LD_PRELOAD=str(ROOT/'tests/udp_fault.so'),FA_TEST_UDP_DROP=drop)
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
            r.start('relay',[str(ROOT/'relay_transfer')]);wait_for(lambda:'RELAY_READY' in r.log('relay'),'relay ready')
        r.client('Host',host=True,mode='udp',drop=mode)
        r.client('Bob',auto=True,mode='udp');r.complete('Bob',timeout=25)
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
        r.start('relay',[str(ROOT/'relay_transfer')]);wait_for(lambda:'RELAY_READY' in r.log('relay'),'relay ready')
        r.command('Bob','get');r.complete('Bob',timeout=15)
        # A newly-bound socket replaces peer 2. Malformed spoofed source is rejected.
        s=socket.create_connection(('127.0.0.1',37405),timeout=2);sockets.append(s)
        inner=frame(4,b'',0)
        bind=frame(0x10,struct.pack('!BH',2,1));wrap=frame(0x11,struct.pack('!BBH',1,3,len(inner))+inner,0)
        for b in bind[:7]:s.sendall(bytes([b]))
        s.sendall(bind[7:]+wrap)
        cmd,body=recv_frame(s);assert cmd==0x13 and body==bytes.fromhex('0012');assert s.recv(1)==b''
        print('PASS relay: real forwarding, fragmented BIND+coalesced WRAP, spoofed src rejected')
    finally:
        for s in sockets:s.close()
        r.close()

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

try:
    invalid_manifests();tcp_faults();resume();udp('once');udp('all');relay_checks()
    print('ALL TRANSFER INTEGRATION TESTS PASS')
finally:
    print('Evidence directory:',BASE)
