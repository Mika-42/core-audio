{
	echo "// ===== main.cpp ====="
	echo '```cpp'
	cat src/main.cpp
	echo '```'

	for f in src/impl/*.cppm; do
		echo "// ===== $f ====="
		echo '```cpp'
		cat "$f"
		echo '```'
	done

	for f in src/utils/*.cppm; do
		echo "// ===== $f ====="
		echo '```cpp'
		cat "$f"
		echo '```'
	done

	echo '```cpp'
	cat src/abstract_core.cppm
	echo '```'
} | wl-copy

