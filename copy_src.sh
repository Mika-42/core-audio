{
	echo "// ===== main.cpp ====="
	echo '```cpp'
	cat main.cpp
	echo '```'

	for f in src/*.cppm; do
		echo "// ===== $f ====="
		echo '```cpp'
		cat "$f"
		echo '```'
	done
} | wl-copy

