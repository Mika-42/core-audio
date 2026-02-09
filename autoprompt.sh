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

	echo "Tu es un expert en C++ et developpement audio, DSP et synthèse sonore.
	Analyse le code ci dessus et indique moi ce qui ne fonctionne pas, ce qui est mal architecturé
	ou qui peut etre améliorer sans aucune perte de qualité en temps réel."
} | wl-copy

