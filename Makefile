build:
	mpic++ -pthread bit_torrent.cpp -o bit_torrent

clean:
	rm -rf bit_torrent
