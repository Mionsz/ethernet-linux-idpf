#!/usr/bin/env bash
# Staged runner for the NIC Port Framework.
# Every stage is timed, logged to .logs/, and reported in a final summary table.
#
#   ./run_framework.sh                      # preparation only (no LLM calls)
#   ./run_framework.sh --model gpt-4.1      # preparation + LLM analysis stages
#   ./run_framework.sh --stages status      # run one stage
#   ./run_framework.sh --help
set -uo pipefail

FRAMEWORK_DIR="$(readlink -f "$(dirname -- "${BASH_SOURCE[0]}")")"
export FRAMEWORK_DIR

# ---------------------------------------------------------------------------
# Defaults (every value is overridable by flag or by pre-exported environment)
# ---------------------------------------------------------------------------
export FRAMEWORK_REPO_DIR="${FRAMEWORK_REPO_DIR:-$(readlink -f "${FRAMEWORK_DIR}/../..")}"
export FRAMEWORK_REPO_SRC_DIR="${FRAMEWORK_REPO_SRC_DIR:-${FRAMEWORK_REPO_DIR}/idpf/src}"
export FRAMEWORK_OUT_DIR="${FRAMEWORK_OUT_DIR:-${FRAMEWORK_REPO_DIR}/.out}"
export KERNEL_BUILD_DIR="${KERNEL_BUILD_DIR:-/usr/src/linux-headers-$(uname -r)}"

MANIFEST="${FRAMEWORK_DIR}/examples/idpf.full.manifest.json"
SPEC_MANIFEST="${FRAMEWORK_DIR}/templates/orchestrator/records/spec_manifest.example.json"
PIPELINE="${FRAMEWORK_DIR}/nic_port_pipeline.py"
ORCH="${FRAMEWORK_DIR}/nic_port_orchestrator.py"
HEADERS="${FRAMEWORK_DIR}/nic_port_headers.py"
OSAL="${FRAMEWORK_DIR}/nic_port_osal.py"
TARGET="${FRAMEWORK_DIR}/nic_port_target.py"
EXTRACTOR_SRC="${FRAMEWORK_DIR}/nic_port_clang_extractor.cpp"

LLVM_DIR="${LLVM_DIR:-/usr/lib/llvm-22}"
CXX="${CXX:-clang++-22}"
LLVM_LIB="${LLVM_LIB:-LLVM-22}"

MODEL=""
JOBS=4
DEVICE_FLOW=0
FORCE=0
RESUME=0
STAGES=""
OSAL_MODE=""
LOCK_TIMEOUT=""
LOCK_ON_TIMEOUT=""
LOG_DIR="${FRAMEWORK_OUT_DIR}/.logs"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"

PREP_STAGES=(preflight extractor compiledb validate osal extract headers diagnose init scope specs status)
LLM_STAGES=(architecture target methods evidence files subsystems final audit)
HARD_PREREQ_STAGES="preflight extractor compiledb validate osal extract init scope"

usage() {
  cat <<'EOF'
Usage: run_framework.sh [options]

Options:
  --manifest PATH        Input manifest        (default: examples/idpf.full.manifest.json)
  --spec-manifest PATH   Specification manifest to ingest
  --out-dir PATH         Framework output root (default: <repo>/.out)
  --repo-dir PATH        Driver repository root
  --src-dir PATH         Driver source directory
  --kernel-dir PATH      Kernel build directory
  --model NAME           LLM model id; enables the analysis stages
  --jobs N               Parallel prompt executions          (default: 4)
  --osal-mode MODE       optional|mandatory; overrides config/rules/osal_rules.json
  --lock-timeout SECONDS How long to wait for the workspace lock (default: policy, 3600)
  --lock-on-timeout ACT  exit|override once the lock wait expires (default: policy, override)
  --device-flow          Authenticate the LLM provider interactively (first run)
  --force                Ignore stage gates when running analysis stages
  --resume               Use 'resume' instead of 'build' for extraction
  --stages LIST          Comma-separated stage list, or 'all'
  -h, --help             Show this help

Stages:
  preparation : preflight extractor compiledb validate osal extract headers
                diagnose init scope specs status
  analysis    : architecture target methods evidence files subsystems final audit

'scope' removes source-OS-only methods from the porting loop and plans the
target-mandated work items; 'target' analyses those work items before 'methods'.

Every run writes a markdown summary to .logs/<run-id>-summary.md, including on
an aborted run, listing what ran, what broke and how to continue.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --manifest)       MANIFEST="$2"; shift 2 ;;
    --spec-manifest)  SPEC_MANIFEST="$2"; shift 2 ;;
    --out-dir)        export FRAMEWORK_OUT_DIR="$2"; shift 2 ;;
    --repo-dir)       export FRAMEWORK_REPO_DIR="$2"; shift 2 ;;
    --src-dir)        export FRAMEWORK_REPO_SRC_DIR="$2"; shift 2 ;;
    --kernel-dir)     export KERNEL_BUILD_DIR="$2"; shift 2 ;;
    --model)          MODEL="$2"; shift 2 ;;
    --jobs)           JOBS="$2"; shift 2 ;;
    --osal-mode)      OSAL_MODE="$2"; shift 2 ;;
    --lock-timeout)   LOCK_TIMEOUT="$2"; shift 2 ;;
    --lock-on-timeout) LOCK_ON_TIMEOUT="$2"; shift 2 ;;
    --device-flow)    DEVICE_FLOW=1; shift ;;
    --force)          FORCE=1; shift ;;
    --resume)         RESUME=1; shift ;;
    --stages)         STAGES="$2"; shift 2 ;;
    -h|--help)        usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

ANALYSIS_ROOT="${FRAMEWORK_OUT_DIR}/idpf-baseline"
mkdir -p "${LOG_DIR}" "${FRAMEWORK_OUT_DIR}"
RUN_LOG="${LOG_DIR}/run-${RUN_ID}.log"

if [[ -z "${STAGES}" ]]; then
  SELECTED=("${PREP_STAGES[@]}")
  [[ -n "${MODEL}" ]] && SELECTED+=("${LLM_STAGES[@]}")
elif [[ "${STAGES}" == "all" ]]; then
  SELECTED=("${PREP_STAGES[@]}" "${LLM_STAGES[@]}")
else
  IFS=',' read -r -a SELECTED <<< "${STAGES}"
fi

declare -a REPORT=()
FAILED=0
BROKEN_STAGE=""
BROKEN_RC=0
SUMMARY_MD="${LOG_DIR}/${RUN_ID}-summary.md"

log()  { printf '%s\n' "$*" | tee -a "${RUN_LOG}"; }
step() { printf '\n== %s ==\n' "$*" | tee -a "${RUN_LOG}"; }

# Run one stage function; capture rc + duration; never abort the whole run.
# REPORT rows: name|result|seconds|rc|logfile
run_stage() {
  local name="$1"
  local fn="stage_${name}"
  if ! declare -F "${fn}" >/dev/null; then
    log "SKIP  ${name} (unknown stage)"
    REPORT+=("${name}|SKIP|0|0|-")
    return 0
  fi
  local log_file="${LOG_DIR}/${RUN_ID}-${name}.log"
  step "STAGE ${name} -> ${log_file}"
  local start rc end
  start=$(date +%s)
  "${fn}" 2>&1 | tee -a "${log_file}" "${RUN_LOG}"
  rc=${PIPESTATUS[0]}
  end=$(date +%s)
  if [[ ${rc} -eq 0 ]]; then
    log "PASS  ${name} ($((end - start))s)"
    REPORT+=("${name}|PASS|$((end - start))|0|${log_file}")
  else
    log "FAIL  ${name} rc=${rc} ($((end - start))s)"
    REPORT+=("${name}|FAIL|$((end - start))|${rc}|${log_file}")
    FAILED=1
    [[ -z "${BROKEN_STAGE}" ]] && { BROKEN_STAGE="${name}"; BROKEN_RC=${rc}; }
  fi
  return ${rc}
}

# How to retry a given stage after a failure.
continuation_hint() {
  case "$1" in
    preflight)  echo "Install the missing tool or correct the missing path, then rerun this stage." ;;
    extractor)  echo "Check the clang/LLVM development packages named in the stage log, then rerun." ;;
    compiledb)  echo "The kernel module build failed or produced no .cmd files; fix the build, then rerun." ;;
    validate)   echo "Correct the manifest or the configuration file it names, then rerun." ;;
    osal)       echo "Correct config/rules/osal_rules.json (mapping or mode), then rerun." ;;
    scope)      echo "Correct config/targets/<profile>.json (exclusion rules or work items), then rerun." ;;
    extract)    echo "Rerun with --resume to continue from the extraction checkpoint instead of restarting." ;;
    headers)    echo "Rerun this stage; if a header cannot be read, fix its permissions or encoding first." ;;
    diagnose)   echo "Informational stage; rerun after the extraction problem it reported is addressed." ;;
    init)       echo "Remove the stale lock in <analysis-root>/orchestration if a previous run was killed, then rerun." ;;
    specs)      echo "Correct the specification manifest passed with --spec-manifest, then rerun." ;;
    status)     echo "Informational stage; safe to rerun at any time." ;;
    audit)      echo "Informational stage; rerun after the analysis stages it audits are complete." ;;
    architecture|methods|evidence|files|subsystems|final)
                echo "Rerun the stage; add --force to bypass the gate, or edit the stage records by hand and rerun." ;;
    target)     echo "Rerun the stage; if a work item is wrong, edit config/targets/<profile>.json, rerun 'scope', then rerun this stage." ;;
    *)          echo "Rerun this stage." ;;
  esac
}

rerun_command() {
  local stage="$1" extra=""
  [[ -n "${MODEL}" ]] && extra+=" --model ${MODEL}"
  [[ ${FORCE} -eq 1 ]] && extra+=" --force"
  [[ "${stage}" == "extract" ]] && extra+=" --resume"
  [[ -n "${OSAL_MODE}" ]] && extra+=" --osal-mode ${OSAL_MODE}"
  echo "./run_framework.sh --manifest ${MANIFEST} --stages ${stage}${extra}"
}

# Markdown sum-up. Always produced: on success, on abort and on interrupt.
write_summary() {
  local ran_names=" "
  local -a rows_ran=() rows_notrun=()
  local row n r s rc lf
  for row in "${REPORT[@]}"; do
    IFS='|' read -r n r s rc lf <<< "${row}"
    ran_names+="${n} "
    rows_ran+=("| \`${n}\` | ${r}$([[ ${rc} -ne 0 ]] && echo " (rc=${rc})") | ${s} | \`${lf}\` |")
  done
  local stage
  for stage in "${SELECTED[@]}"; do
    [[ "${ran_names}" == *" ${stage} "* ]] || rows_notrun+=("${stage}")
  done

  {
    echo "# NIC Port Framework run ${RUN_ID}"
    echo
    echo "| Setting | Value |"
    echo "|---|---|"
    echo "| result | $([[ ${FAILED} -eq 0 ]] && echo "COMPLETED" || echo "STOPPED at \`${BROKEN_STAGE}\`") |"
    echo "| manifest | \`${MANIFEST}\` |"
    echo "| analysis root | \`${ANALYSIS_ROOT}\` |"
    echo "| source root | \`${FRAMEWORK_REPO_SRC_DIR}\` |"
    echo "| osal mode | ${OSAL_MODE:-from configuration} |"
    echo "| model | ${MODEL:-none (prompts rendered only)} |"
    echo "| run log | \`${RUN_LOG}\` |"
    echo
    echo "## Stages executed"
    echo
    echo "| Stage | Result | Seconds | Log |"
    echo "|---|---|---:|---|"
    if [[ ${#rows_ran[@]} -eq 0 ]]; then
      echo "| - | no stage ran | 0 | - |"
    else
      printf '%s\n' "${rows_ran[@]}"
    fi

    if [[ -n "${BROKEN_STAGE}" ]]; then
      echo
      echo "## What broke"
      echo
      echo "| Stage | Exit code | Log |"
      echo "|---|---:|---|"
      echo "| \`${BROKEN_STAGE}\` | ${BROKEN_RC} | \`${LOG_DIR}/${RUN_ID}-${BROKEN_STAGE}.log\` |"
      local broken_log="${LOG_DIR}/${RUN_ID}-${BROKEN_STAGE}.log"
      if [[ -s "${broken_log}" ]]; then
        echo
        echo "Last lines of that stage:"
        echo
        echo '```'
        tail -n 15 "${broken_log}"
        echo '```'
      else
        echo
        echo "That stage produced no output before it stopped."
      fi
    fi

    echo
    echo "## Not run"
    echo
    if [[ ${#rows_notrun[@]} -eq 0 ]]; then
      echo "Every selected stage was attempted."
    else
      echo "| Stage | Reason |"
      echo "|---|---|"
      for stage in "${rows_notrun[@]}"; do
        echo "| \`${stage}\` | run stopped at \`${BROKEN_STAGE}\` before this stage |"
      done
    fi

    echo
    echo "## How to continue"
    echo
    if [[ -n "${BROKEN_STAGE}" ]]; then
      echo "1. $(continuation_hint "${BROKEN_STAGE}")"
      echo "2. Retry the failed stage:"
      echo
      echo '```bash'
      echo "$(rerun_command "${BROKEN_STAGE}")"
      echo '```'
      if [[ ${#rows_notrun[@]} -gt 0 ]]; then
        echo
        echo "3. Then continue with the stages that never ran:"
        echo
        echo '```bash'
        echo "$(rerun_command "$(IFS=,; echo "${rows_notrun[*]}")")"
        echo '```'
      fi
      echo
      echo "If the stage cannot be repaired automatically, edit the inputs it names above by"
      echo "hand and rerun the same command. The runner does not verify that an edit was made."
    else
      echo "The run completed. Review the outputs:"
      echo
      echo "| Artifact | Path |"
      echo "|---|---|"
      echo "| output manifest | \`${ANALYSIS_ROOT}/output_manifest.json\` |"
      echo "| target scope | \`${ANALYSIS_ROOT}/reports/target_scope.md\` |"
      echo "| header inventory | \`${ANALYSIS_ROOT}/reports/header_summary.md\` |"
      echo "| osal policy | \`${ANALYSIS_ROOT}/reports/osal_summary.md\` |"
      echo "| orchestration status | \`${ANALYSIS_ROOT}/orchestration\` |"
    fi
  } > "${SUMMARY_MD}"

  if [[ -d "${ANALYSIS_ROOT}" ]]; then
    mkdir -p "${ANALYSIS_ROOT}/reports"
    cp -f "${SUMMARY_MD}" "${ANALYSIS_ROOT}/reports/run_summary.md" 2>/dev/null || true
  fi
}

SUMMARY_DONE=0
on_interrupt() {
  log "Interrupted."
  FAILED=130
  if [[ -n "${CURRENT_STAGE:-}" && -z "${BROKEN_STAGE}" ]]; then
    BROKEN_STAGE="${CURRENT_STAGE}"
    BROKEN_RC=130
    REPORT+=("${CURRENT_STAGE}|INTERRUPTED|0|130|${LOG_DIR}/${RUN_ID}-${CURRENT_STAGE}.log")
  fi
  exit 130
}

on_exit() {
  [[ ${SUMMARY_DONE} -eq 1 ]] && return 0
  SUMMARY_DONE=1
  write_summary
  log ""
  log "=============================== SUMMARY ==============================="
  cat "${SUMMARY_MD}" | tee -a "${RUN_LOG}"
  log "======================================================================="
  log "summary       : ${SUMMARY_MD}"
  log "full log      : ${RUN_LOG}"
}
trap on_exit EXIT
trap 'on_interrupt' INT TERM

orch() {
  local -a lock=()
  [[ -n "${LOCK_TIMEOUT}" ]] && lock+=(--lock-timeout "${LOCK_TIMEOUT}")
  [[ -n "${LOCK_ON_TIMEOUT}" ]] && lock+=(--lock-on-timeout "${LOCK_ON_TIMEOUT}")
  python3 "${ORCH}" --manifest "${MANIFEST}" "${lock[@]}" "$@"
}

# Render every item of a stage, then execute the ready ones through the provider.
run_analysis_stage() {
  local stage="$1"
  local -a render=(render --root "${ANALYSIS_ROOT}" --stage "${stage}")
  local -a exec=(run --root "${ANALYSIS_ROOT}" --stage "${stage}" --jobs "${JOBS}")
  if [[ ${FORCE} -eq 1 ]]; then
    render+=(--force)
    exec+=(--force)
  fi
  if [[ -n "${MODEL}" ]]; then
    exec+=(--llm-model "${MODEL}")
  fi
  if [[ ${DEVICE_FLOW} -eq 1 ]]; then
    exec+=(--llm-device-flow)
  fi
  orch "${render[@]}" || return $?
  if [[ -z "${MODEL}" ]]; then
    echo "no --model supplied: prompts rendered, execution skipped"
    return 0
  fi
  orch "${exec[@]}"
}

# ---------------------------------------------------------------------------
# Stages
# ---------------------------------------------------------------------------

stage_preflight() {
  echo "FRAMEWORK_DIR      = ${FRAMEWORK_DIR}"
  echo "FRAMEWORK_REPO_DIR = ${FRAMEWORK_REPO_DIR}"
  echo "FRAMEWORK_REPO_SRC_DIR = ${FRAMEWORK_REPO_SRC_DIR}"
  echo "FRAMEWORK_OUT_DIR  = ${FRAMEWORK_OUT_DIR}"
  echo "KERNEL_BUILD_DIR   = ${KERNEL_BUILD_DIR}"
  echo "MANIFEST           = ${MANIFEST}"
  echo "ANALYSIS_ROOT      = ${ANALYSIS_ROOT}"
  local rc=0
  for tool in python3 make "${CXX}"; do
    if command -v "${tool}" >/dev/null 2>&1; then
      echo "tool ok   : ${tool} -> $(command -v "${tool}")"
    else
      echo "tool MISS : ${tool}"; rc=1
    fi
  done
  for path in "${MANIFEST}" "${EXTRACTOR_SRC}" "${FRAMEWORK_REPO_SRC_DIR}" \
              "${KERNEL_BUILD_DIR}/scripts/clang-tools/gen_compile_commands.py" \
              "${LLVM_DIR}/include" "${LLVM_DIR}/lib"; do
    if [[ -e "${path}" ]]; then echo "path ok   : ${path}"; else echo "path MISS : ${path}"; rc=1; fi
  done
  return ${rc}
}

stage_extractor() {
  mkdir -p "${FRAMEWORK_OUT_DIR}"
  "${CXX}" -std=c++17 -O2 "${EXTRACTOR_SRC}" \
    -I"${LLVM_DIR}/include" \
    -D_GNU_SOURCE -D_GLIBCXX_USE_CXX11_ABI=1 \
    -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS \
    -fno-exceptions \
    -L"${LLVM_DIR}/lib" -l"${LLVM_LIB}" -lclang-cpp \
    -o "${FRAMEWORK_OUT_DIR}/nic-port-clang-extractor" \
  && ls -l "${FRAMEWORK_OUT_DIR}/nic-port-clang-extractor"
}

stage_compiledb() {
  make -C "${FRAMEWORK_REPO_DIR}" \
  && python3 "${KERNEL_BUILD_DIR}/scripts/clang-tools/gen_compile_commands.py" \
       -d "${FRAMEWORK_REPO_DIR}" \
       -o "${FRAMEWORK_OUT_DIR}/compile_commands.json" \
  && python3 -c "import json,sys;d=json.load(open('${FRAMEWORK_OUT_DIR}/compile_commands.json'));print('translation units:',len(d));sys.exit(0 if d else 1)"
}

stage_validate() { python3 "${PIPELINE}" --manifest "${MANIFEST}" validate-manifest; }

# Index the OSAL surface. In mandatory mode, rewrite a staged copy of the sources
# onto the OSAL API and retarget the compile database at it before extraction.
# The original repository is never modified.
stage_osal() {
  local -a common=(--manifest "${MANIFEST}")
  [[ -n "${OSAL_MODE}" ]] && common+=(--mode "${OSAL_MODE}")
  python3 "${OSAL}" "${common[@]}" index || return $?
  python3 "${OSAL}" "${common[@]}" plan || return $?

  local mode
  mode="$(python3 "${OSAL}" "${common[@]}" status | awk '/^mode/ {print $3}')"
  if [[ "${mode}" != "mandatory" ]]; then
    echo "OSAL mode is '${mode}': sources are analysed as written; the OSAL surface is context only."
    return 0
  fi

  python3 "${OSAL}" "${common[@]}" stage || return $?
  local staged db
  staged="$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['staged_source_root'])" \
            "${ANALYSIS_ROOT}/manifest/osal_staging.json")" || return $?
  db="${ANALYSIS_ROOT}/manifest/compile_commands.osal.json"
  [[ -d "${staged}" && -f "${db}" ]] || { echo "OSAL staging produced no usable tree"; return 1; }

  cp -f "${FRAMEWORK_OUT_DIR}/compile_commands.json" "${FRAMEWORK_OUT_DIR}/compile_commands.pre_osal.json"
  cp -f "${db}" "${FRAMEWORK_OUT_DIR}/compile_commands.json"
  export FRAMEWORK_REPO_SRC_DIR="${staged}"
  echo "mandatory mode active"
  echo "  source root now : ${FRAMEWORK_REPO_SRC_DIR}"
  echo "  compile database: ${FRAMEWORK_OUT_DIR}/compile_commands.json (original kept as compile_commands.pre_osal.json)"
  python3 "${OSAL}" "${common[@]}" check
}

stage_extract() {
  if [[ ${RESUME} -eq 1 ]]; then
    python3 "${PIPELINE}" --manifest "${MANIFEST}" resume
  else
    python3 "${PIPELINE}" --manifest "${MANIFEST}" build
  fi
}

stage_headers() { python3 "${HEADERS}" --manifest "${MANIFEST}" scan; }

# Remove source-OS-only methods from the porting loop and plan the work items the
# destination OS mandates. Must run before the methods stage.
stage_scope() { python3 "${TARGET}" --manifest "${MANIFEST}" --root "${ANALYSIS_ROOT}" scope; }

stage_diagnose() { python3 "${PIPELINE}" --manifest "${MANIFEST}" diagnose-extraction; }

stage_init() { orch init --root "${ANALYSIS_ROOT}"; }

stage_specs() {
  if [[ ! -f "${SPEC_MANIFEST}" ]]; then
    echo "no specification manifest at ${SPEC_MANIFEST}; skipping"
    return 0
  fi
  orch ingest-spec --root "${ANALYSIS_ROOT}" --spec-manifest "${SPEC_MANIFEST}" \
  && orch capabilities --root "${ANALYSIS_ROOT}"
}

stage_status() { python3 "${PIPELINE}" --manifest "${MANIFEST}" status --verbose; }

stage_architecture() { run_analysis_stage architecture; }
stage_target()       { run_analysis_stage target; }
stage_methods()      { run_analysis_stage methods; }
stage_evidence()     { run_analysis_stage evidence; }
stage_files()        { run_analysis_stage files; }
stage_subsystems()   { run_analysis_stage subsystems; }
stage_final()        { run_analysis_stage final; }

stage_audit() {
  orch verify-evidence --root "${ANALYSIS_ROOT}"
  orch traceability --root "${ANALYSIS_ROOT}" >/dev/null
  orch consistency --root "${ANALYSIS_ROOT}" >/dev/null
  orch audit --root "${ANALYSIS_ROOT}"
}

# ---------------------------------------------------------------------------
# Execute
# ---------------------------------------------------------------------------
log "NIC Port Framework run ${RUN_ID}"
log "log: ${RUN_LOG}"

for stage in "${SELECTED[@]}"; do
  CURRENT_STAGE="${stage}"
  run_stage "${stage}" || {
    if [[ " ${HARD_PREREQ_STAGES} " == *" ${stage} "* ]]; then
      log ""
      log "Aborting: stage '${stage}' is a hard prerequisite."
      [[ "${stage}" == "extract" ]] && python3 "${PIPELINE}" --manifest "${MANIFEST}" diagnose-extraction 2>&1 | tee -a "${RUN_LOG}"
      break
    fi
    log "Continuing after non-fatal stage failure: ${stage}"
  }
done

exit ${FAILED}
