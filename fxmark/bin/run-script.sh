#!/bin/bash

# 반복 횟수를 명령줄 인자에서 받음
repeat_count=$1
workload=$2

echo "$1 : 반복할 $2 의 워크로드 지정"

for ((i=1; i<=repeat_count; i++))
do
    echo "[$2] 반복 $i: root 디렉터리를 삭제합니다."
    # 매 반복마다 root 디렉터리의 모든 내용을 강제로 삭제
    rm -rf root/

    # 현재 시간을 파일 이름에 사용하기 위한 형식 설정
    current_time=$(date "+%Y%m%d_%H%M%S")
    name="${workload}_${current_time}_${i}"

    # 스크립트 실행 결과를 임시 파일에 기록
    ./control_logging.sh start "${name}_hytrack.csv" &

    # ./run-fxmark.py 2>&1 | tee output.log
    python3 -u ./run-fxmark.py --periodic_output > /mnt/tmpfs/"${name}_fxmark.log" 2>&1

    sleep 5
    ./control_logging.sh stop

    # 임시 로그 파일을 /backup/log/ 디렉터리 아래에 현재 시간을 이름으로 하는 파일로 이동
    mv /mnt/tmpfs/"${name}_fxmark.log" /backup/log/"${name}_fxmark.log"
    mv /mnt/tmpfs/"${name}_hytrack.csv" /backup/log/"${name}_hytrack.csv"
done
