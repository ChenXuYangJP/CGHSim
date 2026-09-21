# CGHV wire protocol 1.1

The source of truth is [`include/cgh/wire.hpp`](include/cgh/wire.hpp). The wire format is independent of C++ object layout, Unreal reflection, native enum ordinals, padding, native byte order, and native pointers. Integer fields are unsigned, fixed-width, big-endian. Every optical scalar, vector/quaternion component, sample normal and UV uses IEEE 754 binary64 encoded as its big-endian 64-bit representation. Normals/UVs originating as UE floats are promoted to doubles without losing their original value.

A connection handles one job. There is no compression, authentication, persistent resource cache or session negotiation in version 1.1. Lengths exclude the frame header. A receiver must handle partial sends/receives and validate the header before allocating the body.

## Frame header (32 bytes)

| Offset | Width | Field | Version 1.1 value |
| --- | --- | --- | --- |
| 0 | 4 | Magic | ASCII `CGHV` |
| 4 | 2 | Major version | 1 |
| 6 | 2 | Minor version | 1 |
| 8 | 2 | Message type | Request=1, Result=2, Cancel=3, Error=4 |
| 10 | 2 | Flags | 0 |
| 12 | 4 | Reserved | 0 |
| 16 | 8 | Request ID | Nonzero client-generated correlation ID |
| 24 | 8 | Payload byte length | At most 268435456 (256 MiB) |

Unsupported versions, types, flags and reserved values are rejected. Request IDs in responses and cancellation must match the request. Malformed magic, incomplete frames, missing IDs or disconnected peers may be closed without a response. A well-framed invalid request normally receives an Error frame, then closes.

## Stable enums

All payload enums are `u32`; they must be explicitly mapped by clients, never copied from a native enum's ordinal.

| Enum | Values |
| --- | --- |
| Algorithm | PointFocus=1 |
| Propagation convention | ExpPositiveIKR=1 |
| SLM modulation | PhaseOnly=1, Complex=2 |
| Light source | PlaneWave=1, PointSource=2 |
| Target kind | Point=1, Mesh=2 |
| Result status | DummySuccess=1, PointFocusSuccess=2 |

Unknown enum values are rejected. The CUDA PointFocus solver requires PhaseOnly and PlaneWave. Explicit `--solver dummy` accepts recognized modulation/source metadata without optical computation. `DummySuccess` identifies that fixture; `PointFocusSuccess` identifies the actual PointFocus calculation.

## Request body

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

All floating fields must be finite; pitches/extents/wavelength and dimensions are positive, amplitudes nonnegative. Missing camera metadata may retain its zero defaults. Camera integer dimensions must fit signed 32-bit UE values. SLM dimensions are at most 16384 per axis and 16,777,216 pixels total. Targets are nonempty, resource IDs cannot duplicate, mesh IDs/revisions must be nonzero, and each mesh joins exactly one nonempty cloud by both resource ID and revision. Unreferenced/duplicate clouds are rejected. Target count, cloud count, and the total of point targets plus all mesh samples are bounded by 1,000,000. These are transport/snapshot checks, not a replacement for PointFocus optical validation.

Array bounds are checked against remaining bytes before allocation. Truncated records, trailing bytes, oversized bodies or mismatched revisions fail the complete job. No numerical array is accepted partially.

## Result body

```text
u32 status                  # 1 = DummySuccess, 2 = PointFocusSuccess
u32 convention              # 1 = ExpPositiveIKR
u32 resolution_x
u32 resolution_y
f64 compute_seconds
u64 phase_count
f64 phase_radians[phase_count]
```

The fixed prefix is 32 bytes. `phase_count == resolution_x * resolution_y` and response dimensions equal the corresponding request. Phases are row-major, horizontal column increasing fastest, radians in `[0, 2*pi)`. Compute time is finite and nonnegative. A valid response reconstructs an owned `FCGHSolverResult` with `bSucceeded=true`, an empty error, matching propagation convention, and a complete phase pattern. `DummySuccess` describes transport validation only. `PointFocusSuccess` is returned by the CUDA port of the unchanged CPU reference algorithm. UE retains the distinction when reporting an accepted result.

## Error and cancellation

An Error body is `u32 utf8_byte_count` followed by exactly that many UTF-8 bytes, without a terminator. Length is 1–4096. The server currently emits ASCII messages, a valid UTF-8 subset. Error has no phase data; UE produces `bSucceeded=false` with an empty pattern and the received message.

Cancel has zero payload and the active request ID. It is sent only after the complete Request body, because bytes inside an unfinished body cannot be interpreted as a new frame. Disconnect during any stage is authoritative cancellation. Cancellation races may overlap a response already in flight; the client must discard any response for its cancelled/superseded job. The server monitors Cancel/disconnect while receiving no further request, parsing, delaying, generating, encoding and sending. Unexpected subsequent frames terminate that connection.

## CUDA/V100 extension rules

Version 1.1 readers require exactly major 1/minor 1 and zero flags/reserved bits; they never silently ignore unknown fields. The 1.1 CUDA backend shares the same owned request representation as the earlier 1.0 dummy service. Adding `PointFocusSuccess=2` changes the supported result semantics, so both endpoints now require minor version 1. Version 1.0 and 1.1 peers reject each other explicitly. The unchanged request/result field layout does not imply version compatibility. A numerical result must not masquerade as `DummySuccess`.

Use a new major version for incompatible representations or changed field semantics. A future minor version can add explicitly negotiated capabilities, algorithm identifiers, result status values, precision/compression options, resource caching or new message types. Introduce a capability/hello exchange in that version before optional messages or payload extensions are used; existing 1.1 peers will reject it predictably. Reserve header bits until their semantics, validation and negotiation are documented. Scene schema version is separate from transport version, and new scene schemas require explicit support. Preserve unit conventions, row order, resource revisions and propagation signs when implementing CUDA kernels.
