#!/usr/bin/env python3
"""CGHV 1.4 two-stage thin-lens camera reconstruction against an independent complex oracle."""
import argparse
import cmath
import copy
from dataclasses import dataclass
import math
import os
import struct
import time

from integration import Server, frame, receive
from reconstruction_integration import Scene as SourceScene, IDENTITIES, TAU, rotated, ExternalServer


@dataclass
class Scene(SourceScene):
    camera_position: tuple = (0.21, 0.00013, -0.00021)
    camera_rotation: tuple = (0.0, 0.0, 1.0, 0.0)  # Optical +X faces the SLM.
    focal_length: float = 0.015
    f_number: float = 60.0
    focus_distance: float = 0.21
    sensor_width: int = 7
    sensor_height: int = 5
    sensor_pitch_x: float = 8e-6
    sensor_pitch_y: float = 11e-6
    pupil_width: int = 11
    pupil_height: int = 9


def encode(scene):
    payload = struct.pack("!IIII4dI", 4, 1, scene.width, scene.height,
                          scene.pitch_x, scene.pitch_y, scene.width * scene.pitch_x,
                          scene.height * scene.pitch_y, scene.modulation)
    payload += struct.pack("!6dI4d", scene.wavelength, scene.amplitude, scene.initial_phase,
                           *scene.direction, scene.source, *scene.source_position, 0.23)
    payload += struct.pack("!10dII2dII", *scene.camera_position, *scene.camera_rotation,
                           scene.focal_length, scene.f_number, scene.focus_distance,
                           scene.sensor_width, scene.sensor_height, scene.sensor_pitch_x, scene.sensor_pitch_y,
                           scene.pupil_width, scene.pupil_height)
    phases = scene.phases()
    payload += struct.pack("!Q", len(phases))
    assert len(payload) == 256
    return payload + struct.pack(f"!{len(phases)}d", *phases)


def kernel(q, p, k, area):
    r = math.dist(q, p)
    return area * cmath.exp(1j * k * r) * (q[0] / (TAU * r*r)) * (1/r - 1j*k)


def oracle(scene, indices=None):
    """Direct complex RS sums with math.fsum, circular pupil and quadratic thin lens.

    The error scale propagates absolute source contributions through both stages,
    so cancellation in either stage does not make the comparison ill-conditioned.
    """
    k = TAU / scene.wavelength
    sources = []
    for index, phase in enumerate(scene.phases()):
        row, column = divmod(index, scene.width)
        p = (0.0, (column - (scene.width-1)/2)*scene.pitch_x, ((scene.height-1)/2-row)*scene.pitch_y)
        if scene.source == 1:
            incident = scene.amplitude * cmath.exp(1j*(scene.initial_phase + k*sum(a*b for a,b in zip(scene.direction,p))))
        else:
            r = math.dist(p, scene.source_position)
            incident = scene.amplitude/r * cmath.exp(1j*(scene.initial_phase+k*r))
        sources.append((p, incident*cmath.exp(1j*phase)))
    diameter = scene.focal_length/scene.f_number
    dx, dy = diameter/scene.pupil_width, diameter/scene.pupil_height
    pupil = []
    for row in range(scene.pupil_height):
        for column in range(scene.pupil_width):
            y, z = (column-(scene.pupil_width-1)/2)*dx, ((scene.pupil_height-1)/2-row)*dy
            if (2*y/diameter)**2 + (2*z/diameter)**2 > 1:
                continue
            q = tuple(a+b for a,b in zip(scene.camera_position, rotated(scene.camera_rotation,(0,y,z))))
            contributions = [incident*kernel(q,p,k,scene.pitch_x*scene.pitch_y) for p,incident in sources]
            value = complex(math.fsum(v.real for v in contributions),math.fsum(v.imag for v in contributions))
            lens = cmath.exp(-1j*k*(y*y+z*z)/(2*scene.focal_length))
            pupil.append(((0,y,z),value*lens,math.fsum(abs(v) for v in contributions)))
    v = 1/(1/scene.focal_length - 1/scene.focus_distance)
    if indices is None:
        indices = range(scene.sensor_width*scene.sensor_height)
    results = []
    for index in indices:
        row,column = divmod(index,scene.sensor_width)
        q = (v,(column-(scene.sensor_width-1)/2)*scene.sensor_pitch_x,((scene.sensor_height-1)/2-row)*scene.sensor_pitch_y)
        kernels = [kernel(q,p,k,dx*dy) for p,_,_ in pupil]
        terms = [entry[1]*h for entry,h in zip(pupil,kernels)]
        value = complex(math.fsum(a.real for a in terms),math.fsum(a.imag for a in terms))
        results.append((value,math.fsum(entry[2]*abs(h) for entry,h in zip(pupil,kernels))))
    return results


def receive_result(sock,identity,scene,label):
    kind,actual,payload = receive(sock)
    assert (kind,actual)==(8,identity), f"{label}: expected camera result, got {kind}: {payload!r}"
    status,convention,width,height,seconds,count = struct.unpack("!IIIIdQ",payload[:32])
    assert (status,convention,width,height,count)==(5,1,scene.sensor_width,scene.sensor_height,scene.sensor_width*scene.sensor_height)
    assert math.isfinite(seconds) and seconds>=0 and len(payload)==32+16*count
    samples = tuple(complex(a,b) for a,b in struct.iter_unpack("!dd",payload[32:]))
    assert all(math.isfinite(v.real) and math.isfinite(v.imag) for v in samples)
    assert sock.recv(1)==b"", "Camera result must close without a second frame"
    return samples


def computed(server,scene,label):
    identity=next(IDENTITIES)
    with server.connect() as sock:
        sock.settimeout(30)
        sock.sendall(frame(7,identity,encode(scene)))
        return receive_result(sock,identity,scene,label)


def check_oracle(samples,scene,label,indices=None):
    indices=list(range(len(samples))) if indices is None else indices
    maximum=0.0
    for index,(wanted,scale) in zip(indices,oracle(scene,indices)):
        error=abs(samples[index]-wanted)
        tolerance=6e-8*scale+5e-14
        assert error<=tolerance, f"{label}: pixel {index}: {error:.12g} exceeds {tolerance:.12g}"
        maximum=max(maximum,error)
    print(f"PASS {label}: {len(indices)} oracle pixels, maximum error {maximum:.4g}",flush=True)


def solve(server,scene,label):
    samples=computed(server,scene,label)
    check_oracle(samples,scene,label)
    return samples


def reject_payload(server,payload,label,expected=None):
    identity=next(IDENTITIES)
    with server.connect() as sock:
        sock.settimeout(10)
        sock.sendall(frame(7,identity,payload))
        kind,actual,result=receive(sock)
        assert (kind,actual)==(4,identity),f"{label}: invalid camera request returned success"
        size,=struct.unpack("!I",result[:4])
        assert size>0 and len(result)==4+size
        diagnostic=result[4:].decode()
        if expected: assert expected.lower() in diagnostic.lower(),diagnostic
        assert sock.recv(1)==b"", "Camera error must not publish a partial field"
    print(f"PASS reject {label}: {diagnostic}",flush=True)


def reject(server,scene,label,expected=None):
    reject_payload(server,encode(scene),label,expected)


def multiplied(a,b):
    x,y,z,w=a; X,Y,Z,W=b
    return (w*X+x*W+y*Z-z*Y,w*Y-x*Z+y*W+z*X,w*Z+x*Y-y*X+z*W,w*W-x*X-y*Y-z*Z)


def tilted_scene():
    return Scene(camera_rotation=multiplied((0,math.sin(.17/2),0,math.cos(.17/2)),
                     multiplied((math.sin(.37/2),0,0,math.cos(.37/2)),(0,0,1,0))),
                 direction=(.8,.36,-.48),initial_phase=.79)


def numerical_and_validation(server):
    solve(server,Scene(width=1,height=1,pupil_width=1,pupil_height=1,sensor_width=1,sensor_height=1),"one pupil sample analytical path")
    base=Scene(); normal=solve(server,base,"finite circular pupil and asymmetric sensor")
    for field,value,label in [("focal_length",.012,"changed focal length"),("f_number",85.,"changed aperture"),
                              ("focus_distance",.08,"changed focus distance"),("pupil_width",17,"independent pupil sampling"),
                              ("sensor_pitch_x",17e-6,"sensor pixel spacing")]:
        scene=copy.deepcopy(base);setattr(scene,field,value)
        changed=solve(server,scene,label)
        assert any(abs(a-b)>1e-12 for a,b in zip(changed,normal)),label
    tilted=tilted_scene();solve(server,tilted,"camera tilt and optical roll")
    tilted.source=2;solve(server,tilted,"point-source illumination through tilted lens")
    zero=copy.deepcopy(base);zero.amplitude=0
    assert all(v==0j for v in solve(server,zero,"zero incident field"))
    doubled=copy.deepcopy(base);doubled.amplitude*=2
    assert all(abs(a-2*b)<1e-12 for a,b in zip(solve(server,doubled,"complex field keeps physical amplitude"),normal))
    solve(server,Scene(width=257,height=1,pupil_width=23,pupil_height=23,sensor_width=3,sensor_height=2),"both stages cross contributor batches")
    for field,value,label in [
        ("focal_length",0.,"zero focal length"),("focus_distance",.015,"focus at focal plane"),
        ("focus_distance",.01,"virtual sensor unsupported"),("f_number",0.,"zero f-number"),
        ("sensor_pitch_x",0.,"zero sensor pitch"),("pupil_width",0,"zero pupil samples"),
        ("pupil_width",2049,"pupil sampling limit"),("camera_rotation",(0,0,0,1),"camera faces away from SLM"),
        ("camera_rotation",(0,0,2,0),"nonunit optical rotation"),("camera_position",(-.2,0,0),"pupil behind SLM"),
        ("camera_position",(float('nan'),0,0),"nonfinite camera pose"),("phase",[0.],"SLM count mismatch"),
        ("modulation",2,"complex modulation"),("sensor_width",16385,"sensor axis limit"),
    ]:
        invalid=copy.deepcopy(base);setattr(invalid,field,value);reject(server,invalid,label)
    invalid=copy.deepcopy(base);invalid.sensor_width=4096;invalid.sensor_height=4096
    reject(server,invalid,"sensor field exceeds frame bound")
    invalid=copy.deepcopy(base);invalid.sensor_width=2;invalid.sensor_pitch_x=1e308
    reject(server,invalid,"sensor extent overflow")
    reject_payload(server,encode(base)+b'garbage',"trailing camera bytes")
    reject_payload(server,struct.pack('!I',2)+encode(base)[4:],"observer algorithm in camera request")
    solve(server,base,"valid camera after invalid requests")


def cancellation(executable):
    server=Server(executable,"--max-clients","1")
    try:
        solve(server,Scene(),"camera cancellation warmup")
        cases=[("SLM-to-pupil stage",Scene(width=128,height=64,pupil_width=512,pupil_height=512,sensor_width=1,sensor_height=1)),
               ("pupil-to-sensor stage",Scene(width=1,height=1,pupil_width=128,pupil_height=128,sensor_width=1024,sensor_height=1024))]
        for label,scene in cases:
            identity=next(IDENTITIES)
            with server.connect() as sock:
                sock.settimeout(5);sock.sendall(frame(7,identity,encode(scene)));time.sleep(.15)
                start=time.monotonic();sock.sendall(frame(3,identity))
                assert sock.recv(1)==b'',f"{label}: cancellation returned partial camera output"
                elapsed=time.monotonic()-start;assert elapsed<2.,f"{label}: cancellation took {elapsed:.3f}s"
            time.sleep(.15);solve(server,Scene(),f"camera recovery after cancellation during {label}")
        with server.connect() as sock:
            sock.sendall(frame(7,next(IDENTITIES),encode(cases[-1][1])));time.sleep(.15)
        time.sleep(.3);solve(server,Scene(),"camera disconnect drains both-stage resources")
    finally:server.close()


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('executable',nargs='?');parser.add_argument('--port',type=int)
    args=parser.parse_args()
    if bool(args.executable)==bool(args.port):parser.error('provide executable or --port')
    if args.port:numerical_and_validation(ExternalServer(args.port))
    else:
        server=Server(args.executable)
        try:numerical_and_validation(server)
        finally:server.close()
        cancellation(args.executable)
        server=Server(args.executable,env=dict(os.environ,CUDA_VISIBLE_DEVICES='-1'))
        try:reject(server,Scene(),'no GPU has no fallback','CUDA')
        finally:server.close()
    print('CUDA camera optical, validation and lifecycle checks passed',flush=True)


if __name__=='__main__':main()
