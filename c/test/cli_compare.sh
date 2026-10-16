#!/bin/sh
# Runs bzip2 1.0.8's bzip2.c built against stock libbz2 and against bz2-ng on
# the same command lines and inputs, and compares stdout bytes, stderr text
# (program path normalised) and exit status.  Inputs: the bzip2 samples,
# generated text, concatenated streams, trailing garbage and zeros, truncated
# streams, randomised-block streams, header-only, non-bzip2 and empty files,
# and every bit flip of one sample.  Further arguments are files that the
# bz2-ng program needs beside it (on Windows, the DLL it loads).
# usage: cli_compare.sh stock-bzip2 bz2ng-bzip2 cli_tool scratch-directory [file...]
set -u
S=$1
N=$2
TOOL=$3
W=$4
shift 4
REF=$(cd "$(dirname "$0")/reference" && pwd)
rm -rf "$W"
mkdir -p "$W"
cd "$W" || exit 2
# Both programs run as "bzip2", which they print in their messages; a Windows
# executable keeps its extension.
case $S in
*.exe) x=.exe ;;
*) x= ;;
esac
mkdir -p stock ng
cp "$S" stock/bzip2$x
cp "$N" ng/bzip2$x
for f in "$@"; do
	cp "$f" ng/
done
S=$W/stock/bzip2$x
N=$W/ng/bzip2$x
cp "$REF"/sample*.bz2 "$REF"/sample*.ref .
# Generated inputs; the comparison is meaningless without them.
gen() { # output file, then the cli_tool arguments; stdin passed through
	out=$1
	shift
	if ! "$TOOL" "$@" > "$out" || [ ! -s "$out" ]; then
		echo "cli_compare: could not generate $out"
		exit 2
	fi
}
gen text.ref text 300000
gen rand_text.bz2 rand-compress < text.ref
gen rand_s1.bz2 rand-compress < sample1.ref
cat sample1.bz2 sample2.bz2 sample3.bz2 > concat.bz2
cat sample1.bz2 sample3.bz2 > garbage.bz2
printf 'trailing garbage after the streams' >> garbage.bz2
cat sample3.bz2 > zeros.bz2
head -c 100 /dev/zero >> zeros.bz2
head -c 20000 sample1.bz2 > trunc.bz2
cat sample3.bz2 > trunc_concat.bz2
head -c 30000 sample2.bz2 >> trunc_concat.bz2
printf 'BZh9' > header_only.bz2
printf 'not bzip2 at all' > notbz.bz2
: > empty.bz2
pass=0
fail=0
run() { # label, then bzip2 arguments; input from $IN
	label=$1
	shift
	"$S" "$@" < "$IN" > out.s 2> err.s
	es=$?
	"$N" "$@" < "$IN" > out.n 2> err.n
	en=$?
	# libbz2's sorter reports falling back to its second sorter at -vv;
	# bz2-ng has no such sorter (a documented divergence).
	# After "*Possible* reason follows." bzip2.c calls perror, which prints
	# whatever errno an earlier call left, failed or not: a heap that cannot
	# grow, as under QEMU's user mode, leaves ENOMEM in one library's run and
	# not in the other's. That line says nothing about the library, so it is
	# not compared; the message around it still is.
	sed -e "s|$S|bzip2|g" -e '/too repetitive; using fallback sorting algorithm/d' \
		-e '/[*]Possible[*] reason follows[.]$/{' -e n -e 's/.*/bzip2: (errno text)/' -e '}' err.s > err.s2
	sed -e "s|$N|bzip2|g" \
		-e '/[*]Possible[*] reason follows[.]$/{' -e n -e 's/.*/bzip2: (errno text)/' -e '}' err.n > err.n2
	if [ $es -eq $en ] && cmp -s out.s out.n && cmp -s err.s2 err.n2; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "DIFF $label: exit $es/$en"
		diff err.s2 err.n2 | head -5
	fi
}
# Compression: every level, small-input flags, verbose block lines.
for IN in sample1.ref sample2.ref sample3.ref text.ref; do
	for lv in 1 2 3 4 5 6 7 8 9; do
		run "$IN -$lv" -c -$lv
	done
	run "$IN -vv" -c -vv
	run "$IN -9 -q" -c -9 -q
done
# Decompression and testing.
for IN in *.bz2; do
	run "$IN -dc" -dc
	run "$IN -dsc" -dsc
	run "$IN -t" -t
	run "$IN -tvvvv" -tvvvv
	run "$IN -dcq" -dcq
done
# Every bit flip of sample3.bz2, decompressed.
n3=$(wc -c < sample3.bz2)
i=0
while [ $i -lt $((n3 * 8)) ]; do
	gen flip.bz2 flip sample3.bz2 $i
	IN=flip.bz2
	run "sample3 flip $i -dc" -dc
	i=$((i + 1))
done
echo "cli_compare: $pass identical, $fail different"
[ $fail -eq 0 ]
