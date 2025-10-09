CPP_FILES = alsa/alsa-core-audio.cpp 

create-build-dir:
	mkdir -p build 
build-alsa: create-build-dir
	g++ $(CPP_FILES) main.cpp -lasound -o build/main -Iabstract -Wall -Wextra -Werror -Wpedantic -std=c++23
