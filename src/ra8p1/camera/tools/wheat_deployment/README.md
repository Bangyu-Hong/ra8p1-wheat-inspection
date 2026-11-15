# Wheat host deployment reference

This directory freezes the PC-side contract used before RA8P1 firmware
integration. It does not contain a model, dataset, generated tensors, or RUHMI
compiler output.

The teammate-facing Phase-A training and artifact contract for the official
224x224 YOLOX-Tiny route is documented in
[`RA8P1_YOLOX_TINY_WHEAT_TRAINING_HANDOFF.md`](RA8P1_YOLOX_TINY_WHEAT_TRAINING_HANDOFF.md).

## YOLOX full224 diagnostic calibration

The delivered YOLOX model resizes a complete 4:3 GWHD view to a 224 input. It
is useful for a `full224-chain-smoke` of the INT8/MERA/Ethos-U55 deployment
chain, but it is not evidence for the final `tile224/stride179` VGA model.

Prepare exactly 200 train-only calibration tensors after proving exact parity
against all 20 delivered golden inputs:

```powershell
E:\Anaconda\anaconda_install_way\envs\yolo\python.exe `
  workspace/camera/tools/run_wheat_deployment.py prepare-yolox-calibration `
  --dataset-root "数据集/global-wheat-codalab-official" `
  --split-manifest "models/YOLOX_Tiny_GWHD_224_训练完成产物_20260810/split_manifest.csv" `
  --golden-zip "models/YOLOX_Tiny_GWHD_224_训练完成产物_20260810/golden_20_images.zip" `
  --output-dir "output/yolox_tiny_full224_intake/calibration_200"
```

The command refuses a non-empty output directory. Generated tensors and the
evidence manifest stay under `output/` and are not product source artifacts.

The received `full224` candidate completed the deployment-chain diagnostic but
was rejected for firmware. Its immutable result is in
`manifests/yolox_tiny_gwhd_full224_candidate.json`. A fresh saved-MERA reload
on all 20 golden inputs retained only 426 of 4,082 FP32 candidates and 166 of
671 post-NMS detections; NMS candidate-index recall was 2.09% and count MAE
against FP32 was 25.25. The output remained finite and nonconstant, so this is
quantized detection-recall loss rather than a runtime crash.

Allocation also remains mixed. The three Vela regions contain 299 NPU
operators and zero CPU operators inside those regions, but surrounding C code
quantizes and slices the YOLOX Focus input, executes both nearest-neighbor
feature-map Resize operations, and dequantizes the output. Do not integrate or
flash this candidate. The next handoff must be a true `tile224/stride179`
model, hardened for the same pinned INT8 path and requalified with saved-model
detection and complete-allocation evidence.

## Fixed contracts

- Camera input: `640x480 RGB565-BE`.
- Source input: RGB float32 `NCHW [1,3,640,640]`, normalized by `255.0`.
- Geometry: do not resize the camera frame; add 80 rows of RGB 114 above and
  below it.
- Original source output: float32 `[1,5,8400]`, anchor-free
  `xywh + confidence`.
- Derived compiler-candidate outputs: `boxes` float32 `[1,4,8400]` and
  `scores` float32 `[1,1,8400]`. The host decoder reconstructs the original
  logical tensor with `np.concatenate([boxes, scores], axis=1)`.
- Postprocess: no extra sigmoid, fixed top-score candidate pool, coordinate
  restoration/clipping, and one global NMS pass.

The original smoke-test contract is in
`manifests/yolo11n_wheat_full640.json`. Its source SHA-256 is
`f0f06bf0032e62f14da5882f87e5440585796f9710de67482775fcb99202270c`.

The original YOLOv8n replacement is frozen separately in
`manifests/yolov8n_wheat_full640_candidate.json`. It has the same input,
preprocessing, output, and postprocessing contracts, so it uses the same host
reference without an architecture-specific branch. Its ONNX graph has no
`MatMul` or C2PSA block, removing the known YOLO11n conversion blocker.

The pinned RUHMI run completed quantization and code generation but rejected
that single-output ABI. `output0` had PSNR `8.649` and score `86.352`; the final
score tensor had FP32 maximum `0.785` but quantized maximum `0.000` because box
coordinates and confidence shared one INT8 range. MERA produced four
`C_CODEGEN` and three `ARM_ETHOS_U55` regions. Each Vela region reported zero
CPU operators, but that statement does not include the four C regions.

The full640 split-output derivative is frozen in
`manifests/yolov8n_wheat_full640_split_outputs.json`. The deterministic
`split-terminal-concat-v1` transform removes only `/model.22/Concat_5` and
exposes its existing box and score inputs. It does not change network
calculations. Its RUHMI run was also rejected: the quantized score maximum was
`0.118`, below the unchanged `0.25` threshold. The generated ONNX belongs under
`output/` and is not committed.

The GWHD manifest selects five box-count quantiles from each of the seven
annotated domains. It records the seven annotation hashes and all 35 selected
image hashes. Three quantiles per domain, 21 images in total, are also golden
inputs.

## Commands

Run from the repository root with the existing Python 3.10 `yolo` interpreter:

```powershell
$python = "E:\Anaconda\anaconda_install_way\envs\yolo\python.exe"
$tool = "workspace\camera\tools\run_wheat_deployment.py"
$candidate = "models\wheat_yolov8n_640\weights\best.onnx"
$candidateContract = "workspace\camera\tools\wheat_deployment\manifests\yolov8n_wheat_full640_candidate.json"
$derived = "output\ruhmi_yolov8n_full640\derived\yolov8n_wheat_full640_split_outputs.onnx"
$derivedContract = "workspace\camera\tools\wheat_deployment\manifests\yolov8n_wheat_full640_split_outputs.json"
$dataset = Get-ChildItem -Path . -Directory -Recurse -ErrorAction SilentlyContinue `
  -Filter "global-wheat-codalab-official" | `
  Select-Object -First 1 -ExpandProperty FullName

& $python $tool verify-model `
  --model $candidate `
  --contract $candidateContract
& $python $tool derive-split-output-model `
  --model $candidate `
  --output $derived
& $python $tool verify-model `
  --model $derived `
  --contract $derivedContract
& $python $tool verify-manifest --dataset-root $dataset
& $python $tool prepare-calibration `
  --dataset-root $dataset `
  --output-dir "output\ruhmi_yolov8n_full640\calibration_fp32"
```

Each calibration file has shape `[1,3,640,640]` and dtype `float32`, which is
the form expected when RUHMI loads a directory of `.npy` samples. The tool uses
a deterministic center 4:3 crop and Pillow bilinear resize to approximate a VGA
camera view from each square GWHD source image. It then applies the exact
production letterbox and normalization.

Run one FP32 ONNX golden reference:

```powershell
& $python $tool run-reference `
  --model $derived `
  --contract $derivedContract `
  --image "$dataset\arvalis_1\98d9469c-a518-4801-9ed3-75aec0f9a78a.png" `
  --output-dir "output\ruhmi_yolov8n_full640\golden_fp32_split_00"
```

The output contains the exact input tensor, reconstructed raw output,
decoded/NMS JSON, tool versions, timings, and hashes. On the fixed golden input,
the derived outputs concatenated to a bitwise-identical copy of original
`output0` with maximum absolute difference `0.0`. Both reference runs produced
the same raw-output and detection hashes. This is host structural evidence, not
CPU-fallback acceptance and not a board result. All generated output stays
under the root `output` directory and must not be committed.

Run focused tests:

```powershell
& "workspace\camera\tests\run_wheat_host_tests.ps1"
```

## Historical full640 RUHMI compiler gate

The pinned public compiler source is Renesas tag `Release-2026-06-19`, commit
`6c5aad901a1a41e28f6e306bfc35c44659e89502`. It provides MERA
`2.6.0+pkg.4815`, Vela `4.2.0`, and a Windows command-line workflow. Install it
in a dedicated Python 3.10 environment outside this repository. Do not install
it into the training `yolo` environment.

The following command records the already completed full640 split-output run.
It is retained for reproduction and must not be treated as a pending retry:

```powershell
& "F:\Renesas\RUHMI\venv-2.6.0\Scripts\python.exe" `
  "F:\Renesas\RUHMI\ruhmi-framework-mcu\scripts\mcu_compile.py" `
  "F:\Desktop\RenesasCup\output\ruhmi_yolov8n_full640\derived\yolov8n_wheat_full640_split_outputs.onnx" `
  "F:\Desktop\RenesasCup\output\ruhmi_yolov8n_full640\compiler_split_outputs" `
  --npu --quantize `
  --calib-data "F:\Desktop\RenesasCup\output\ruhmi_yolov8n_full640\calibration_fp32" `
  --calib-num 35 --memory-threshold 64 --ref-data
```

`--memory-threshold 64` disables the script's generic 0.8 MiB automatic OSPI
heuristic for this first report. It does not assert that the model fits internal
MRAM. The generated allocation and memory report decides whether a second,
explicit `--external` OSPI compile is required. Any unsupported operator, CPU
partition, compiler warning, or memory overflow is a gate failure and must be
retained in the raw log.

The run exposed separate `boxes` and `scores` tensors but still produced a
quantized score maximum of only `0.118`; therefore it failed the unchanged
`0.25` confidence gate. Successful code generation did not advance firmware.

## Full320 decoded split-output rejection

The dedicated full320 decoded split-output PTQ gate was run once on 2026-08-09
and is rejected. `split-terminal-concat-v1` generated
`output/ruhmi_yolov8n_320/derived/yolov8n_wheat_full320_split_outputs.onnx`
from the fixed full320 export. The derived model is 12,125,201 bytes with
SHA-256 `99d83f727a9c929a278140903c1703e62db9a0b901967a818708ac496d39c5e9`.
On the fixed golden VGA tensor, concatenating `[1,4,2100]` boxes and
`[1,1,2100]` scores is bitwise equal to the source `[1,5,2100]` output. Both
have raw-output SHA-256
`ea6b67fb0a5c4fef25a5faef9307c529e398814990ce5f66aa9487395ac90733`,
score maximum `0.738399088382721`, 235 candidates at confidence `0.25`, and
33 detections after NMS.

The RUHMI venv `Scripts` directory was prepended to `PATH` so Vela was found.
The one compile generated four `C_CODEGEN` and three `ARM_ETHOS_U55` regions.
The Vela regions contain 156, 27, and 192 NPU operators respectively, each
with zero CPU operators. Their SRAM requirements are 800.00, 300.00, and
2,627.06 KiB. The compile process returned nonzero only after generation when
the Windows GBK console could not encode the framework's checkmark character;
the complete log is
`output/ruhmi_yolov8n_320/compiler_split_outputs.log` with SHA-256
`39640781e291b4da982826ea4011863bad3d02a06d5ee87f76a60bbd8ced0a5a`.

The saved 3,690,770-byte quantized `model.mera` has SHA-256
`678fb34c61301bfbab3fc3120a92e93dad84c0eca5cc3d285ed1c0d7057604e9`.
It was loaded with `ModelLoader.from_quantized_mera()` and run in a fresh
`MERAInterpreter` deployment on the same golden tensor. Its score maximum was
only `0.1348143219947815`, below the unchanged `0.25` threshold, so it produced
zero candidates and zero detections. This is the authoritative saved-model
result; the compiler table's `0.499` score maximum came from its separate
evaluation sample. No threshold reduction, observer sweep, raw-head retry,
firmware integration, or CPU substitute is permitted. Detailed generated
evidence is in `output/ruhmi_yolov8n_320/full320_split_outputs_ptq_rejection.json`.
No accepted full320 split-output manifest was created.

## QAT replacement handoff

The next model route is a newly trained quantization-aware candidate with the
same decoded split-output ABI. This does not authorize another observer sweep,
threshold reduction, raw-head retry, or CPU inference substitution.

The public Renesas repository was checked on 2026-08-09. Its latest release is
still `Release-2026-06-19`; public `main` was
`ebc6497651750188561cfb57291efda821e82163`. The public compiler describes its
MERA path as post-training static INT8 quantization, and `mcu_compile.py`
explicitly warns that a pre-quantized INT8 ONNX cannot be deployed directly.
Therefore a generic INT8/QDQ ONNX is not an accepted handoff for this pinned
toolchain.

The bounded experimental route is:

```text
QAT fine-tuning from a trainable YOLOv8n full320 checkpoint
  -> export a fixed FP32 ONNX, batch 1, RGB NCHW [1,3,320,320]
  -> decoded outputs: boxes [1,4,2100], scores [1,1,2100]
  -> no dynamic axes, FP16, embedded NMS, or combined box/score output
  -> validate FP32 parity and unchanged 0.25 detections
  -> run the one pinned MERA --npu --quantize gate
  -> load the saved model.mera in a fresh MERAInterpreter
  -> accept only if scores and detections remain structurally plausible
```

Because MERA still performs the final static quantization, this is a
QAT-hardened PTQ experiment rather than native import of a QAT INT8 graph. The
handoff must include the QAT framework/version and configuration, source/base
weight hashes, dataset YAML and split identity, training logs, `best.pt`, the
fixed FP32 split-output ONNX, and fixed-image FP32 results. Firmware remains on
the explicit `replacement-model-pending` backend until the saved MERA model
passes that host gate.

The 2026-08-09 `QAT_YOLOv8n_320_handoff_20260809` intake is frozen in
`manifests/yolov8n_wheat_full320_qat_handoff.json`. Independent inspection
qualified the delivered split model as fixed FP32 RGB/NCHW
`[1,3,320,320] -> boxes [1,4,2100], scores [1,1,2100]`. Both ONNX files pass
the checker, contain no FP16, dynamic interface axes, or embedded NMS, and are
byte-identical in initializers and non-packaging nodes. The split model removes
only the terminal combined-output Concat and adds two output Identity nodes.

The existing fixed project golden tensor was re-run with ONNX Runtime 1.21.1.
Split reassembly was bitwise equal to the combined model, score maximum was
`0.7145407795906067`, 245 candidates remained at `0.25`, and NMS retained 32
at IoU `0.45`. This qualifies the immutable FP32/static intake only. The
handoff does not contain that golden tensor, and its own reported validation
JPEG is absent, so the committed manifest records those as evidence gaps rather
than treating the report as an independent check.

Training traceability is improved: the supplied dataset YAML and 3,422-row
manifest freeze 2,735 train, 339 validation, and 348 test identities with
aggregate SHA-256
`d2c4d7dac7f2c91a0f3a75358e76c05215be22037a1b40d91a91f7200299a627`.
All identities map one-to-one to the seven local annotated GWHD domains, and
the reported base checkpoint hash matches the existing immutable full320
checkpoint. The generated JPEG/label training tree is not present for content
rehashing, the upstream `yolov8n.pt` hash is absent, and training/accuracy
metrics were not independently rerun. They remain teammate-run evidence.

`provenance_and_fp32_report.json` matches the delivered ONNX and checkpoint
hashes. `smoke_report.json` names older ONNX hashes and is retained only as a
stale export smoke record.

The single permitted pinned MERA qualification then completed with 35 frozen
calibration tensors and exit code zero. Its log SHA-256 is
`43c405ba110c34b335028086009aee51e7f8a50133490cc31ad78091029299fe`.
MERA produced four `C_CODEGEN` and three `ARM_ETHOS_U55` regions; the Vela
regions contain 156, 27, and 190 NPU operators with zero CPU operators and use
800.00, 300.00, and 2,627.06 KiB SRAM respectively. Successful allocation is
not model acceptance: the compiler report shows `_model_22_Div_1` collapsing
from FP32 range `0.304..39.658` to quantized `0.0..0.0`.

The retained `io_desc.json` fixes output order as boxes then scores, excluding
an output-order decoding error. The supplied training script attaches
activation fake quant only to Conv/Conv-BN-SiLU outputs; it does not cover the
terminal DFL/dist2bbox Div, Add, Sub, Concat, or Mul operations. The observed
Div collapse is therefore a decoded-tail QAT coverage failure, not a threshold
or output-mapping problem.

The saved 3,689,080-byte `model.mera`, SHA-256
`92d68a0dcde30be55862fda7bb0fb261a56009bc253304c11f468f46f9ed8f4b`,
was loaded through `ModelLoader.from_quantized_mera()` in a fresh interpreter
and run on the same fixed golden. Its score maximum was `0.3678655922` and
eleven scores remained above `0.25`, but all eleven boxes had zero decoded
centers, so valid geometry and NMS detections were both zero. This QAT
candidate is rejected. Do not lower the threshold, run a second compiler
variant, substitute CPU deployment, or integrate it into firmware; the backend
remains `replacement-model-pending`.

## User-approved QAT predecode candidate

The rejected decoded-output QAT manifest above remains immutable. The approved
predecode candidate reuses its combined FP32 source, SHA-256
`a59903d06fb9bd8f7e1a69b755edd00bcca7018a0736f91a54fdb232dd0971d2`,
and stops the graph at `/model.22/Split`. The guarded transform removes exactly
the 34-node DFL, dist2bbox/stride, class-sigmoid, and decoded-output tail. It
exposes `regression_logits [1,64,2100]` and
`class_logits [1,1,2100]`; the direct outputs are produced by the retained
Split. A deterministic wrapper adds one unique Identity producer per output for
the pinned compiler ABI.

This QAT export uses ONNX opset 13 and supplies the static `[64,1]` split as a
second Constant input. It also contains a fake-quantized fixed DFL projection,
not exact integer bins `0..15`. The 16 values and SHA-256
`59caad87de2582a742029e5c0a6bc3540ca50ff92de2a65aa300c0af3af126db`
are frozen in
`manifests/yolov8n_wheat_full320_qat_predecode_candidate.json`. Host and future
MCU reconstruction must use those exact constants to reproduce the immutable
decoded source.

Generate the candidates and all FP32 evidence without invoking RUHMI:

```powershell
$python = "E:\Anaconda\anaconda_install_way\envs\yolo\python.exe"
$tool = "workspace\camera\tools\run_wheat_deployment.py"
$source = "models\QAT_YOLOv8n_320_handoff_20260809\model_and_reports\model_combined_fp32.onnx"
$golden = "output\ruhmi_yolov8n_320\golden_fp32_00\input_fp32.npy"
$output = "output\ruhmi_yolov8n_320_qat_predecode"

& $python $tool qualify-predecode-candidate `
  --model $source `
  --expected-source-sha256 a59903d06fb9bd8f7e1a69b755edd00bcca7018a0736f91a54fdb232dd0971d2 `
  --golden-input $golden `
  --output-dir $output
```

The direct candidate is 12,067,502 bytes, SHA-256
`0a7d09348cc6c6d8343d5c6fb3a9e967bd7b1a23a6eb2246f67ea1913d4a3c5b`.
The identity-wrapped candidate is 12,067,919 bytes, SHA-256
`154f55cef66bf2aa9d108e0c1b9110e83299ba987e97275338f4dbdf8103424d`.
Both pass ONNX checker, strict shape inference, fixed-interface FP32, no-FP16,
and no-NMS checks. Repeated transforms produce identical hashes.

ONNX Runtime 1.21.1 with graph optimization disabled qualified the fixed golden,
an all-zero tensor, and a deterministic uint8-ramp tensor. All three direct and
Identity logits were bitwise equal. Source-versus-host reconstruction stayed
within absolute tolerance `0.000125` with maximum difference
`0.00006103515625`; retained candidate index sequences/counts and NMS index
sequences/counts matched at confidence `0.25` and IoU `0.45`. The cases retained
`245/32`, `0/0`, and `3/2` candidate/NMS counts respectively. Machine-readable
NPY hashes and evidence are under
`output/ruhmi_yolov8n_320_qat_predecode/fp32_qualification.json`.

There is a known frontend risk. Older non-QAT full320 direct and
identity-wrapped predecode graphs both crashed `fe_onnx_cli` during import with
exit status `3221226505`; the immutable logs are
`output/ruhmi_yolov8n_320/compiler_predecode_outputs.log` and
`output/ruhmi_yolov8n_320/compiler_predecode_identity_outputs.log`. Those runs
do not qualify or reject this QAT graph. The parent session owns a frontend-only
preflight on the pinned identity-wrapped candidate. Only if that preflight
passes may it perform exactly one pinned `--npu --quantize` gate with the
unchanged 35 calibration tensors. No threshold reduction, observer sweep,
second compiler variant, CPU neural-network runtime, or firmware integration is
allowed around a failure. Current deployment status is
`FP32-qualified/pending exactly one pinned RUHMI gate`.

### Final pinned RUHMI result

The parent-owned input-description preflight passed with MERA
`2.6.0+pkg.4815` and reported `images float32 [1,3,320,320]`. This check was
too shallow to qualify the Quantizer path: it reads the ONNX input description,
but the later fresh Quantizer deployment uses a separate converted-only
frontend invocation.

The single permitted pinned compile then ran with framework commit
`6c5aad901a1a41e28f6e306bfc35c44659e89502`, the identity-wrapped candidate,
all 35 unchanged calibration tensors, `--npu --quantize`, memory threshold 64,
and `--ref-data`. All 35 tensors were loaded, but the converted-only
`fe_onnx_cli` import failed with decimal exit status `3221226505`
(`0xC0000409`) before `Quantizer.calibrate()`, quantization, Vela allocation,
C generation, or `model.mera` save completed.
The 2,616-byte log has SHA-256
`952bfafb8fc956419f327e3d03aab3c043a71a62d0d76e0747493887d0c71025`.

MERA produced an intermediate optimized ONNX before the crash. It removed the
two output Identity nodes, again exposing both outputs directly from
`/model.22/Split`. The optimized graph passes ONNX checker and golden
ONNX Runtime evaluation within the frozen FP32 tolerance, but the native MERA
frontend still terminates before a deployable model exists. No `.mera` file or
allocation report was generated.

This candidate is therefore rejected on the pinned public RUHMI toolchain even
though its FP32 partition contract remains qualified. The one-compile allowance
is consumed. Do not retry a Slice/single-output variant, lower thresholds,
change observers, substitute a CPU neural-network runtime, or integrate this
candidate into firmware. A future attempt requires a separately reviewed
compiler/toolchain or model-boundary decision. Firmware remains
`replacement-model-pending`.

## Six-output final-Conv gate

The separately approved final-Conv candidate is frozen in
`manifests/yolov8n_wheat_full320_qat_six_head_candidate.json`. Its guarded
transform exposes six learned Conv outputs directly in fixed order:

```text
regression_s8  [1,64,40,40]  class_s8  [1,1,40,40]
regression_s16 [1,64,20,20]  class_s16 [1,1,20,20]
regression_s32 [1,64,10,10]  class_s32 [1,1,10,10]
```

Host assembly flattens each NCHW scale row-major and visits `s8`, `s16`, then
`s32`; future MCU code must process scale descriptors directly rather than
allocate an extra FP32 concatenation buffer. The derived model is 12,065,297
bytes with SHA-256
`8cdfaa986fb5fe3e841d3c1182ca04075e3fa15a6462be49727b8c83f8ad7e0b`.
Every output is produced directly by its frozen final Conv. Internal backbone
Split nodes remain, but there is no output Split or Identity.

The fixed golden, zero, and deterministic uint8-ramp host cases reproduced the
decoded source within `0.000125`; the maximum difference was
`0.00006103515625`. Candidate/NMS counts were `245/32`, `0/0`, and `3/2` with
exact index parity. Repeated qualification produced the same report SHA-256
`1ac2d10da093e778687a0a3e3d8f9aa2666b4f6a880979d36012c49a55cd7059`.

The exact Quantizer-constructor preflight then passed on MERA
`2.6.0+pkg.4815`, producing a 15,225,770-byte MIR without invoking calibration,
quantization, Vela, or code generation. Therefore the single full pinned gate
was allowed and consumed. It loaded all 35 frozen tensors, saved a 3,636,616-byte
`model.mera` with SHA-256
`301f97207813c03e12adeacf5c6a285270ee76458a5347c44d12d797764322cd`,
and generated C. The command returned 1 only because the Windows GBK console
could not encode the script's final check-mark character.

The saved model was reloaded through `ModelLoader.from_quantized_mera()` in a
fresh interpreter and rejected. On the unchanged golden and threshold `0.25`,
the FP32 reference had 245 candidates and 32 NMS detections, while the saved
model had only 8 and 2. Its score maximum fell from `0.714540899` to
`0.353022367`; only four candidate indices overlapped and no NMS indices
overlapped. All six raw tensors remained finite and non-collapsed, so this is a
severe quantized detection-recall loss rather than the prior zero-geometry
failure.

Allocation is also incompatible with the no-CPU-network-runtime contract. The
three Vela regions contain 156, 27, and 135 NPU operators with zero CPU
operators inside those regions, but four surrounding C regions remain. Two of
them execute backbone nearest-neighbor Resize operations on the Cortex-M85;
the others quantize/transpose the input and dequantize all six outputs. This is
not an NPU-only neural-network path.

The candidate is rejected and firmware remains `replacement-model-pending`.
Do not lower thresholds, compile another boundary variant, accept the CPU
Resize fallback, integrate generated sources, or flash this model. The next
candidate needs training/export changes that improve final-head INT8 behavior
and remove or replace the NPU-incompatible backbone upsampling path.

## ConvTranspose operator gate

The proposed no-Resize YOLOv8n replacement was stopped before training. The
reproducible gate is implemented by `run_convtranspose_smoke.py` and frozen in
`manifests/convtranspose_operator_gate.json`.

The preparation command exposes the inputs of the two immutable source Resize
nodes, runs all 35 frozen camera-preprocessed tensors, builds channel-diagonal
`ConvTranspose(kernel=2,stride=2)` smoke models, and verifies their FP32 output
against two-axis `np.repeat`:

```powershell
python workspace/camera/tools/run_convtranspose_smoke.py prepare `
  --source-model models/QAT_YOLOv8n_320_handoff_20260809/model_and_reports/model_combined_fp32.onnx `
  --source-calibration-dir output/ruhmi_yolov8n_320/calibration_fp32 `
  --output-dir output/ruhmi_convtranspose_operator_gate
```

The formal CLI fixes the calibration count at 35 and rejects any source model
whose SHA-256 is not
`a59903d06fb9bd8f7e1a69b755edd00bcca7018a0736f91a54fdb232dd0971d2`.
It does not expose a reduced-count qualification option.

Compile each prepared model in a fresh directory, then reload the saved model
and inspect the complete allocation. This example shows the larger shape; use
`c128_20_to_40` for the second shape:

```powershell
$gateRoot = "output/ruhmi_convtranspose_operator_gate"
$shape = "c256_10_to_20"
$model = "$gateRoot/$shape/convtranspose_$shape.onnx"
$calibration = "$gateRoot/$shape/calibration_fp32"
$compilerOutput = "$gateRoot/$shape/compiler"
$savedModel = "$compilerOutput/convtranspose_${shape}_NPU_quantized/quantization/model.mera"
$ruhmiPython = "F:/Renesas/RUHMI/venv-2.6.0/Scripts/python.exe"
$compiler = "F:/Renesas/RUHMI/ruhmi-framework-mcu/scripts/mcu_compile.py"
$env:PATH = "F:/Renesas/RUHMI/venv-2.6.0/Scripts;$env:PATH"

& $ruhmiPython $compiler $model $compilerOutput `
  --npu --quantize --calib-data $calibration --calib-num 35 `
  --memory-threshold 64 --ref-data

$savedHash = (Get-FileHash -Algorithm SHA256 $savedModel).Hash.ToLowerInvariant()
& $ruhmiPython workspace/camera/tools/run_convtranspose_smoke.py saved-gate `
  --model $savedModel --expected-model-sha256 $savedHash --spec $shape `
  --calibration-dir $calibration --output-dir "$gateRoot/$shape/saved_model_gate"

python workspace/camera/tools/run_convtranspose_smoke.py inspect-allocation `
  --compiler-output-dir $compilerOutput `
  --output-report "$gateRoot/$shape/allocation_gate.json"
```

The pinned compiler can return 1 after creating all core artifacts because its
final check-mark cannot be encoded by the Windows GBK console. Verify the saved
model, generated C, log, and allocation files before interpreting that exit.
`inspect-allocation` returns 4 for a correctly detected allocation rejection.

Both real shapes passed ONNX checker, static-shape validation, and all 35 FP32
comparisons with maximum absolute difference `0.0`. Both pinned RUHMI compiles
also generated `model.mera` and C/Vela sources. Fresh saved-model reloads were
numerically usable: worst normalized RMSE was `0.029322` for
`[1,256,10,10] -> [1,256,20,20]` and `0.002767` for
`[1,128,20,20] -> [1,128,40,40]`.

The allocation gate nevertheless rejects both shapes. In each generated
`model_subgraphs.json`, the learned `TFLiteTransposeConv2dBias` is in a
`C_CODEGEN` region. The two `ARM_ETHOS_U55` regions contain only
`TFLiteTranspose` layout conversions. Generated CPU kernels are 799,138 and
205,859 bytes respectively. Seeing two Vela regions with one NPU operator each
must not be interpreted as NPU execution of ConvTranspose.

Do not start the custom ConvTranspose YOLOv8n training, integrate these models,
or flash them. The next route is NanoDet-Plus intake and a fixed-shape RUHMI
feasibility gate; MobileNetV2-SSDLite is the second fallback. Firmware remains
`replacement-model-pending`.
