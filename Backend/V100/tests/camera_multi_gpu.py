#!/usr/bin/env python3
"""Camera reconstruction preserves ordered complex sums across GPU partitions in both stages."""
import concurrent.futures
import copy
import os
import sys
import time
from integration import frame
from multi_gpu_integration import visible_devices
from reconstruction_multi_gpu import server_process, identical
from camera_integration import Scene, IDENTITIES, computed, solve, check_oracle, encode, receive_result, tilted_scene


def main(executable):
    devices=visible_devices()
    with server_process(executable,dict(os.environ,CUDA_VISIBLE_DEVICES=devices[0][0])) as single, server_process(executable) as multiple:
        point=tilted_scene();point.source=2
        fixtures=[('one sample and more GPUs than outputs',Scene(width=1,height=1,pupil_width=1,pupil_height=1,sensor_width=1,sensor_height=1)),
                  ('odd pupil and sensor partitions',tilted_scene()),('point illumination and camera roll',point),
                  ('even pupil and sensor grids',Scene(pupil_width=12,pupil_height=8,sensor_width=8,sensor_height=6))]
        for label,scene in fixtures:
            expected=solve(single,scene,'single GPU: '+label)
            identical(computed(multiple,scene,label),expected,label)
        # Stage one has 131841 pupil samples: split rows and tiles on both devices.
        pupil=Scene(width=3,height=1,pupil_width=513,pupil_height=257,sensor_width=2,sensor_height=1)
        expected=computed(single,pupil,'large pupil baseline')
        check_oracle(expected,pupil,'pupil partition and tile boundaries')
        identical(computed(multiple,pupil,'large pupil split'),expected,'first-stage tiles and second-stage contributor batches')
        # Stage two crosses tiles and contributor batches, including 256/512-source boundaries in stage one.
        sensor=Scene(width=513,height=1,pupil_width=23,pupil_height=23,sensor_width=513,sensor_height=257)
        baseline=computed(single,sensor,'large sensor baseline')
        indices=[0,512,513,65535,65536,65919,65920,131071,131072,len(baseline)-1]
        check_oracle(baseline,sensor,'sensor global coordinates and ordered source batches',indices)
        identical(computed(multiple,sensor,'large sensor split'),baseline,'second-stage tile partitions')
        variants=[]
        for index in range(3):
            scene=copy.deepcopy(sensor);scene.sensor_width=31+2*index;scene.sensor_height=17+2*index;scene.initial_phase+=.23*(index+1)
            variants.append(scene)
        expected=[computed(single,scene,'concurrent baseline') for scene in variants]
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            futures=[pool.submit(computed,multiple,scene,'concurrent camera') for scene in variants]
            for index,future in enumerate(futures):identical(future.result(timeout=30),expected[index],f'concurrent camera {index}')
        heavy=Scene(width=1,height=1,pupil_width=128,pupil_height=128,sensor_width=1024,sensor_height=1024)
        identity,surviving_identity=next(IDENTITIES),next(IDENTITIES)
        with multiple.connect() as cancelled,multiple.connect() as survivor:
            cancelled.settimeout(5);survivor.settimeout(30)
            cancelled.sendall(frame(7,identity,encode(heavy)));time.sleep(.15)
            survivor.sendall(frame(7,surviving_identity,encode(sensor)));time.sleep(.02)
            start=time.monotonic();cancelled.sendall(frame(3,identity))
            assert cancelled.recv(1)==b'', 'Cancelled two-stage camera returned partial samples'
            assert time.monotonic()-start<2.,'Multi-GPU camera cancellation exceeded bound'
            identical(receive_result(survivor,surviving_identity,sensor,'camera survivor'),baseline,'camera cancellation preserves concurrent two-stage job')
        identical(computed(multiple,sensor,'post-cancellation'),baseline,'camera GPU resources recover after cancellation')
    print('Multiple-GPU camera parity, stages, tiles, concurrency and cancellation passed',flush=True)


if __name__=='__main__':
    assert len(sys.argv)==2,'usage: camera_multi_gpu.py /path/to/cgh_v100_server'
    main(sys.argv[1])
