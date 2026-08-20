#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${HEEREMA_BUILD_DIR:-${project_dir}/.build}"
wled_dir="${WLED_DIR:-${build_dir}/WLED}"
wled_ref="v16.0.1"
environment="an_penta_deca_smart_buttons"
usermod_version="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "${project_dir}/library.json")"

mkdir -p "${build_dir}" "${project_dir}/dist"

if [[ ! -d "${wled_dir}/.git" ]]; then
  git clone --depth 1 --branch "${wled_ref}" https://github.com/wled/WLED.git "${wled_dir}"
fi

actual_ref="$(git -C "${wled_dir}" describe --tags --exact-match 2>/dev/null || true)"
if [[ "${actual_ref}" != "${wled_ref}" ]]; then
  echo "WLED_DIR must be checked out at ${wled_ref}; found ${actual_ref:-an untagged commit}." >&2
  exit 1
fi

escaped_project_dir="$(printf '%s' "${project_dir}" | sed 's/[&|]/\\&/g')"
sed "s|@USERMOD_PATH@|${escaped_project_dir}|g" \
  "${project_dir}/build/platformio_override.ini" \
  > "${wled_dir}/platformio_override.ini"

if command -v pio >/dev/null 2>&1; then
  pio_command=(pio)
else
  venv_dir="${build_dir}/platformio-venv"
  if [[ ! -x "${venv_dir}/bin/pio" ]]; then
    python3 -m venv "${venv_dir}"
    "${venv_dir}/bin/python" -m pip install --upgrade pip
    "${venv_dir}/bin/python" -m pip install platformio==6.1.19
  fi
  pio_command=("${venv_dir}/bin/pio")
fi

"${pio_command[@]}" run -d "${wled_dir}" -e "${environment}"

firmware_name="WLED_${wled_ref#v}_An-Penta-Deca_Heerema-Smart-Buttons_v${usermod_version}.bin"
cp "${wled_dir}/.pio/build/${environment}/firmware.bin" \
  "${project_dir}/dist/${firmware_name}"

if command -v sha256sum >/dev/null 2>&1; then
  (cd "${project_dir}/dist" && sha256sum "${firmware_name}" > "${firmware_name}.sha256")
else
  (cd "${project_dir}/dist" && shasum -a 256 "${firmware_name}" > "${firmware_name}.sha256")
fi

echo "Firmware: ${project_dir}/dist/${firmware_name}"
cat "${project_dir}/dist/${firmware_name}.sha256"
