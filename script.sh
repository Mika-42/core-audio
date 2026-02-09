g++ -std=c++26 -g -Og -fmodules -c src/error.cppm -o obj/error.o
g++ -std=c++26 -g -Og -fmodules -c src/config.cppm -o obj/config.o
g++ -std=c++26 -g -Og -fmodules -c src/block.cppm -o obj/block.o
g++ -std=c++26 -g -Og -fmodules -c src/abstract_core.cppm -o obj/abstract_core.o
g++ -std=c++26 -g -Og -fmodules -c src/alsa_impl.cppm -o obj/alsa_impl.o
g++ -std=c++26 -g -Og -fmodules main.cpp obj/*.o -lasound -o app

