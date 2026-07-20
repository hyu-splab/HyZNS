#!/bin/bash

force_clean=0

# 명령행 인자 처리
while getopts "f" opt; do
  case $opt in
    f)
      force_clean=1
      ;;
    \?)
      echo "Invalid option: -$OPTARG" >&2
      exit 1
      ;;
  esac
done

# wrap 파일에서 디렉토리 이름을 추출하는 함수
get_wrap_directory() {
    local wrap_file=$1
    if [ -f "$wrap_file" ]; then
        # wrap 파일에서 directory= 라인을 찾아 값만 추출
        local dir=$(grep "^directory = " "$wrap_file" | cut -d'=' -f2 | tr -d ' ')
        echo "$dir"
    fi
}

# 서브프로젝트 정리 함수
cleanup_subproject() {
    local wrap_file="subprojects/$1.wrap"
    local default_dir="subprojects/$1"
    
    # wrap 파일이 존재하면 지정된 디렉토리 이름 사용
    if [ -f "$wrap_file" ]; then
        local wrap_dir=$(get_wrap_directory "$wrap_file")
        if [ ! -z "$wrap_dir" ]; then
            if [ -d "subprojects/$wrap_dir" ]; then
                echo "Removing $wrap_dir from subprojects..."
                rm -rf "subprojects/$wrap_dir"
            fi
        fi
    fi
    
    # 기본 이름의 디렉토리도 확인
    if [ -d "$default_dir" ]; then
        echo "Removing $1 from subprojects..."
        rm -rf "$default_dir"
    fi
}

# -f 옵션이 주어진 경우 기존 라이브러리 제거
if [ $force_clean -eq 1 ]; then
    echo "Force clean: removing existing libnvme..."
    sudo rm -f /usr/local/lib/x86_64-linux-gnu/libnvme*
    echo "Updating library cache..."
    sudo ldconfig
fi

# 빌드 디렉토리 정리
if [ -d ".build" ]; then
    echo "Removing build directory..."
    rm -rf ".build"
fi

# 서브프로젝트들 정리
# cleanup_subproject "json-c"
# cleanup_subproject "libnvme"

echo "Setting up new build environment..."
meson setup .build --wrap-mode=forcefallback

echo "Building..."
ninja -C .build

echo "Installing..."
sudo ninja -C .build install

if [ $force_clean -eq 1 ]; then
    echo "Updating library cache after installation..."
    sudo ldconfig
fi

echo "Build and installation complete!"