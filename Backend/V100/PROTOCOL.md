# CGHV wire protocol 1.4

The source of truth is [`include/cgh/wire.hpp`](include/cgh/wire.hpp). The wire format is independent of C++ object layout, Unreal reflection, native enum ordinals, padding, native byte order, and native pointers. Integer fields are unsigned, fixed-width, big-endian. Every optical scalar, vector/quaternion component, sample normal and UV uses IEEE 754 binary64 encoded as its big-endian 64-bit representation. Normals/UVs originating as UE floats are promoted to doubles without losing their original value.

A connection handles one job. There is no compression, authentication, persistent resource cache or session negotiation in version 1.4. Lengths exclude the frame header. A receiver must handle partial sends/receives and validate the header before allocating the body.

## Frame header (32 bytes)

| Offset | Width | Field | Version 1.4 value |
| --- | --- | --- | --- |
| 0 | 4 | Magic | ASCII `CGHV` |
| 4 | 2 | Major version | 1 |
| 6 | 2 | Minor version | 4 |
| 8 | 2 | Message type | Request=1, Result=2, Cancel=3, Error=4, ReconstructionRequest=5, ReconstructionResult=6, CameraReconstructionRequest=7, CameraReconstructionResult=8 |
| 10 | 2 | Flags | 0 |
| 12 | 4 | Reserved | 0 |
| 16 | 8 | Request ID | Nonzero client-generated correlation ID |
| 24 | 8 | Payload byte length | At most 268435456 (256 MiB) |

Unsupported versions, types, flags and reserved values are rejected. Request IDs in responses and cancellation must match the request. Malformed magic, incomplete frames, missing IDs or disconnected peers may be closed without a response. A well-framed invalid request normally receives an Error frame, then closes.

## Stable enums

All payload enums are `u32`; they must be explicitly mapped by clients, never copied from a native enum's ordinal.

| Enum | Values |
| --- | --- |
| Algorithm | PointFocus=1, ObserverPlaneReconstruction=2, PointFocusInverseR=3, CameraThinLens=4 |
| Propagation convention | ExpPositiveIKR=1 |
| SLM modulation | PhaseOnly=1, Complex=2 |
| Light source | PlaneWave=1, PointSource=2 |
| Target kind | Point=1, Mesh=2 |
| Result status | DummySuccess=1, PointFocusSuccess=2, ReconstructionSuccess=3, PointFocusInverseRSuccess=4, CameraReconstructionSuccess=5 |

Unknown enum values are rejected. Type 1 accepts PointFocus and PointFocusInverseR; type 5 accepts only ObserverPlaneReconstruction; type 7 accepts only CameraThinLens. Both CUDA PointFocus modes require PhaseOnly and PlaneWave. Explicit `--solver dummy` accepts recognized modulation/source metadata for either PointFocus mode without optical computation, and rejects all reconstruction jobs with an Error. `DummySuccess` identifies that solver fixture; `PointFocusSuccess` identifies the original PointFocus calculation; `PointFocusInverseRSuccess` identifies the inverse-distance calculation; `ReconstructionSuccess` identifies a complete complex observer field; `CameraReconstructionSuccess` identifies a complete complex sensor field. The server's default `--solver cuda` dispatches each request type to its corresponding CUDA backend.

Both the Unreal client and TCP server must use exactly version 1.4. Rebuild the client and backend image/executable together; 1.0, 1.1, 1.2 and 1.3 peers are rejected, even though the existing PointFocus payload layouts are unchanged.

## PointFocus / PointFocusInverseR Request body (type 1)

Fields appear in exactly this order. `vec3` means three `f64` values `(x,y,z)`; `quat` means four `f64` values `(x,y,z,w)`. Array counts appear once before contiguous elements, without alignment or padding.

| Block | Fields in wire order |
| --- | --- |
| Solver | `u32 scene_schema_version` (2), `u32 algorithm`, `u32 convention` |
| SLM | `u32 resolution_x`, `u32 resolution_y`, `f64 pixel_pitch_x_m`, `f64 pixel_pitch_y_m`, `f64 active_width_m`, `f64 active_height_m`, `u32 modulation` |
| Reconstruction light | `f64 wavelength_m`, `f64 amplitude`, `f64 initial_phase_rad`, `vec3 direction_slm`, `u32 source`, `vec3 position_slm_m`, `f64 polarization_angle_rad` |
| Camera | `vec3 optical_position_slm_m`, `vec3 forward_direction_slm`, `f64 focal_length_m`, `f64 f_number`, `f64 focus_distance_m`, `f64 sensor_width_m`, `f64 sensor_height_m`, `u32 output_resolution_x`, `u32 output_resolution_y` |
| Targets | `u32 target_count`, then target records |
| Point clouds | `u32 cloud_count`, then cloud records |

Each target record is 92 bytes:

```text
u64 resource_id
u64 revision
quat rotation_slm
vec3 position_slm_m
f64 amplitude
f64 phase_rad
u32 kind
```

Each cloud contains:

```text
u64 resource_id
u64 revision
u32 point_count
point[point_count]
```

Each point record is 80 bytes:

```text
vec3 position_local_m
vec3 normal_local
f64 amplitude
f64 phase
f64 u
f64 v
```

The canonical `FCGHSolverInput` numerical snapshot is preserved, including camera metadata, resource identity/revision and local sample fields. Compatibility aliases (`TargetId`, `PositionSLM`, `GeometryResourceId`, `GeometryRevision`) are omitted because their canonical values are already represented. Mesh triangle geometry is not part of `FCGHSolverInput` and is not sent. Point-cloud amplitude and phase already include target parameters; solvers must not apply them twice.

Positions/pitches/extents use meters, phases/polarization use radians. The SLM rigid frame is `+X` optical normal, `+Y` increasing column, `+Z` decreasing row. Target positions/quaternions place rigid local mesh samples in the SLM frame; resource samples already include actor/component scale. Propagation convention 1 means `U(r) ∝ exp(+i*k*r)`: focusing subtracts `k*r` and incident phase from target phase. The CUDA solver implements this convention; future solvers must preserve it or reject it explicitly.

Algorithm 1 preserves the original phase-only PointFocus behavior: each target contributes its specified amplitude to the complex source sum. Algorithm 3 weights that amplitude by inverse propagation distance `1/r` for each SLM pixel, before computing the sum's phase. For emitter `j`, both use source phase `phase_j - k*r_j`; algorithm 1 uses weight `amplitude_j`, while algorithm 3 uses `amplitude_j/r_j`. The selected phase is the argument of the resulting sum minus the incident plane-wave phase. Distances in inverse-distance mode must be finite and positive. Positive common weight scaling leaves the phase unchanged, and a single contributing point therefore has the same phase in both modes. Mesh sample amplitudes/phases are already baked and remain applied once. This algorithm selection changes no payload fields or sample ordering.

All floating fields must be finite; pitches/extents/wavelength and dimensions are positive, amplitudes nonnegative. Missing camera metadata may retain its zero defaults. Camera integer dimensions must fit signed 32-bit UE values. SLM dimensions are at most 16384 per axis and 16,777,216 pixels total. Targets are nonempty, resource IDs cannot duplicate, mesh IDs/revisions must be nonzero, and each mesh joins exactly one nonempty cloud by both resource ID and revision. Unreferenced/duplicate clouds are rejected. Target count, cloud count, and the total of point targets plus all mesh samples are bounded by 1,000,000. These are transport/snapshot checks, not a replacement for PointFocus optical validation.

Array bounds are checked against remaining bytes before allocation. Truncated records, trailing bytes, oversized bodies or mismatched revisions fail the complete job. No numerical array is accepted partially.

## PointFocus / PointFocusInverseR Result body (type 2)

```text
u32 status                  # 1 = DummySuccess, 2 = PointFocusSuccess, 4 = PointFocusInverseRSuccess
u32 convention              # 1 = ExpPositiveIKR
u32 resolution_x
u32 resolution_y
f64 compute_seconds
u64 phase_count
f64 phase_radians[phase_count]
```

The fixed prefix is 32 bytes. `phase_count == resolution_x * resolution_y` and response dimensions equal the corresponding request. Phases are row-major, horizontal column increasing fastest, radians in `[0, 2*pi)`. Compute time is finite and nonnegative. A valid response reconstructs an owned `FCGHSolverResult` with `bSucceeded=true`, an empty error, matching propagation convention, and a complete phase pattern. `DummySuccess` describes transport validation only. `PointFocusSuccess` is returned only for algorithm 1 and `PointFocusInverseRSuccess` only for algorithm 3. Clients reject a computed status that does not match the requested algorithm, even if its phase array is otherwise valid. `ReconstructionSuccess=3` and `CameraReconstructionSuccess=5` are invalid in this result type. `DummySuccess` remains explicitly marked as a transport fixture for either solver algorithm; it cannot be mistaken for a numerical result.


## Observer ReconstructionRequest body (type 5)

This independent message carries the active SLM phase pattern and optical geometry without targets, point clouds, camera metadata or a solver scene-schema field. Its fixed prefix is 224 bytes. The SLM and light blocks use exactly the same field layouts and enum values as PointFocus.

| Offset | Block | Fields in wire order |
| --- | --- | --- |
| 0 | Reconstruction | `u32 algorithm` (2), `u32 convention` (1) |
| 8 | SLM | `u32 resolution_x`, `u32 resolution_y`, `f64 pixel_pitch_x_m`, `f64 pixel_pitch_y_m`, `f64 active_width_m`, `f64 active_height_m`, `u32 modulation` |
| 52 | Light | `f64 wavelength_m`, `f64 amplitude`, `f64 initial_phase_rad`, `vec3 direction_slm`, `u32 source`, `vec3 position_slm_m`, `f64 polarization_angle_rad` |
| 136 | Observer | `u32 resolution_x`, `u32 resolution_y`, `f64 pixel_pitch_x_m`, `f64 pixel_pitch_y_m`, `vec3 position_slm_m`, `quat rotation_slm` |
| 216 | Phase array | `u64 phase_count`, then `f64 phase_radians[phase_count]` starting at offset 224 |

`phase_count` must equal the SLM width times height. These are the actual finite SLM radians, including negative and unwrapped values; encoding does not wrap, quantize or normalize them. They use the SLM row-major order described above. All geometry and light values must be finite, dimensions/pitches/extents/wavelength positive, and light amplitude nonnegative. Recognized source and modulation enums are checked by the codec; the CUDA backend additionally validates the supported optics.

The observer is independent of the SLM grid. Its rigid quaternion maps observer-local vectors into SLM-local vectors, and `position_slm_m` locates the observer origin in meters. Local optical normal is `+X`, columns increase along `+Y`, and rows increase along `-Z`. Before applying that rigid pose, observer pixel centers are:

```text
(0,
 (column - (resolution_x - 1)/2) * pixel_pitch_x_m,
 ((resolution_y - 1)/2 - row) * pixel_pitch_y_m)
```

Each axis is bounded by 16384. SLM sample count remains bounded by 16,777,216; observer sample count is bounded by `(268435456 - 32) / 16 = 16,777,214` so the complete complex result fits one frame. The decoder checks dimensions, count bounds and exact remaining bytes before allocating the phase array. Invalid values, truncation, trailing bytes and cancellation reject the complete request and preserve the previously accepted output object.

This mode reconstructs a scalar complex optical field on an observer plane using PhaseOnly modulation and a PlaneWave or PointSource reconstruction light. Camera thin-lens reconstruction uses the separate type 7 request below. Unknown algorithms must be rejected rather than routed to another mode.

## Observer ReconstructionResult body (type 6)

```text
u32 status                  # 3 = ReconstructionSuccess only
u32 convention              # 1 = ExpPositiveIKR
u32 resolution_x            # observer width, not SLM width
u32 resolution_y            # observer height, not SLM height
f64 compute_seconds
u64 sample_count
repeat sample_count times:
    f64 real
    f64 imaginary
```

The fixed prefix is 32 bytes, followed by exactly 16 bytes per complex sample. `sample_count == resolution_x * resolution_y`; response dimensions must equal the requested observer dimensions. Real and imaginary components are finite IEEE binary64 in observer row-major order, with no amplitude normalization or phase quantization. Signed zero is preserved by the codec. Compute time is finite and nonnegative. The exact result length, pixel limit, status, convention and every component are validated before a result is published; clients additionally verify request correlation and dimensions. Type 2 solver results and their statuses are never accepted as reconstructed fields, and type 6 fields are never accepted as solved SLM patterns.

## CameraReconstructionRequest body (type 7)

The camera request has a 256-byte prefix and its own optical camera block. It does not extend or change the camera metadata in solver requests.

| Offset | Block | Fields in wire order |
| --- | --- | --- |
| 0 | Reconstruction | `u32 algorithm` (4), `u32 convention` (1) |
| 8 | SLM | Same 44-byte SLM block as type 5 |
| 52 | Light | Same 84-byte light block as type 5 |
| 136 | Camera pose | `vec3 optical_position_slm_m`, `quat optical_rotation_slm` |
| 192 | Lens | `f64 focal_length_m`, `f64 f_number`, `f64 focus_distance_m` |
| 216 | Sensor | `u32 output_resolution_x`, `u32 output_resolution_y`, `f64 pixel_pitch_x_m`, `f64 pixel_pitch_y_m` |
| 240 | Pupil | `u32 pupil_resolution_x`, `u32 pupil_resolution_y` |
| 248 | Phase array | `u64 phase_count`, then `f64 phase_radians[phase_count]` starting at offset 256 |

SLM phases, light fields and sensor dimensions have the same finite-value/count limits as observer reconstruction. Pupil dimensions are 1–2048 per axis, at most 4,194,304 samples. Lens focal length `f`, f-number `N` and sensor pitches must be finite and positive; focus distance `s` must be finite and greater than `f`. Derived pupil diameter `D=f/N`, pupil pitches `D/pupil_resolution_x` and `D/pupil_resolution_y`, their area, and image distance `v=f/(1-f/s)` must be finite and positive. The camera quaternion must be unit length. There is no implicit normalization, image flipping or change to SLM phase samples.

The optical position is the thin lens center. Camera optical `+X` faces the scene; `+Y` increases sensor columns and `-Z` increases rows. The physical sensor lies at camera-local `X=-v`. All SLM pixel centers must lie in the camera's forward half-space; all pupil rectangle pixel centers must lie in SLM-local `X>0`. Tilt and roll are carried by the full optical quaternion. A camera looking away, a pupil crossing the SLM plane, a virtual image distance or nonfinite geometry fails the complete job.

The scalar calculation uses two midpoint Rayleigh–Sommerfeld I propagations with `exp(+i*k*r)` and `k=2*pi/wavelength`. For source samples `P=(0,y,z)` and an output sample `Q` in a forward propagation frame, each stage accumulates

```text
DeltaArea * U(P) * exp(+i*k*r) * Q.x/(2*pi*r^2) * (1/r - i*k),  r=|Q-P|.
```

Stage one propagates the illuminated SLM phase field to the full pupil rectangle placed by the camera optical pose. Pupil sample centers use the same centered row/column coordinates as the observer plane. Samples outside `(2*y/D)^2 + (2*z/D)^2 <= 1` are blocked. Inside the circular pupil, multiply the complex field by the ideal paraxial thin-lens transmission `exp(-i*k*(y^2+z^2)/(2*f))`. Stage two uses pupil-local sources `(0,y,z)` and sensor samples `(v,sensor_y,sensor_z)` in an abstract forward propagation frame, retaining the camera's sensor Y/Z axes. It returns the raw optical image, including optical inversion. Source order is row-major, blocked pupil samples are omitted, and both stages use ordered FP64 compensated sums.

The lens is ideal and paraxial: aberrations, polarization, material dispersion, reflections and sensor pixel-area integration are absent. Pupil sampling is a numerical quadrature setting, independent of sensor sampling. A coarse grid can severely alias phase and produce false image replicas; users must increase pupil resolution and check convergence for their optical geometry. A default 64 x 64 pupil grid is a coarse starting point, not a guarantee of accuracy. The thin-lens sign and lens equation follow [TU Delft's coherent imaging treatment](https://qiweb.tudelft.nl/aoi/coherentimaging/coherentimaging/).

CUDA partitions output samples across all visible devices independently in each stage. Source fields are replicated per device, each output retains the same ordered source accumulation, and there is no cross-device complex reduction. Stage-one workers and transfers drain before pupil sources are replaced for stage two. Either-stage cancellation, disconnect or error drains all workers and publishes no partial field. There is no CPU or dummy fallback. Compute time covers both stages and intermediate pupil construction.

## CameraReconstructionResult body (type 8)

The layout is identical to type 6's 32-byte prefix and row-major binary64 real/imaginary pairs, but status must be `CameraReconstructionSuccess=5` and dimensions must match the requested sensor. Type 6 accepts only status 3; type 8 accepts only status 5. Clients reject a result of the other reconstruction mode even when dimensions and bytes otherwise agree. Sensor complex amplitude is preserved without normalization or exposure/tone mapping.

## Error and cancellation

An Error body is `u32 utf8_byte_count` followed by exactly that many UTF-8 bytes, without a terminator. Length is 1–4096. The server currently emits ASCII messages, a valid UTF-8 subset. Error has no numerical data; UE produces a failed job with an empty phase pattern or complex field and the received message.

Cancel has zero payload and the active request ID. It is sent only after the complete solver, observer or camera request body, because bytes inside an unfinished body cannot be interpreted as a new frame. Disconnect during any stage is authoritative cancellation. Cancellation races may overlap a response already in flight; the client must discard any response for its cancelled/superseded job. The server monitors Cancel/disconnect while receiving no further request, parsing, delaying, generating, encoding and sending. Unexpected subsequent frames terminate that connection.

## CUDA/V100 extension rules

Version 1.4 readers require exactly major 1/minor 4 and zero flags/reserved bits; they never silently ignore unknown fields. Version 1.4 adds CameraThinLens algorithm 4, camera message types 7/8 and success status 5. Existing solver and observer payload layouts and enum values remain unchanged. Both endpoints must be rebuilt: versions 1.0 through 1.4 reject different versions explicitly. A computed result must never masquerade as `DummySuccess`, and the dummy transport fixture cannot return a reconstructed field.

Use a new major version for incompatible representations or changed field semantics. A future minor version can add explicitly negotiated capabilities, algorithm identifiers, result status values, precision/compression options, resource caching or new message types. Introduce a capability/hello exchange in that version before optional messages or payload extensions are used; existing 1.4 peers will reject it predictably. Reserve header bits until their semantics, validation and negotiation are documented. Scene schema version is separate from transport version, and new scene schemas require explicit support. Preserve unit conventions, row order, resource revisions and propagation signs when implementing CUDA kernels.
