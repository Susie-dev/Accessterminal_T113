#!/bin/bash

# 1. 批量转换所有wav为同名pcm
for f in *.wav; do
    [ -e "$f" ] || continue
    out="${f%.wav}.pcm"
    sox "$f" -t raw -r 16000 -b 16 -c 1 "$out"
    echo "Converted $f -> $out"
done

# 2. 批量推送所有pcm到开发板的/tmp目录
for p in *.pcm; do
    [ -e "$p" ] || continue
    adb push "$p" /tmp/
    echo "Pushed $p to /tmp/"
done
