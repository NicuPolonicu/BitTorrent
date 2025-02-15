#include <mpi.h>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <unordered_map>


#define TRACKER_RANK 0
#define MAX_PEERS    10
#define MAX_FILES    10
#define MAX_FILENAME 15
#define HASH_SIZE    32
#define MAX_CHUNKS   100

// Tipurile de mesaje folosite
#define BEGIN_COMMUNICATION       0
#define READY_TO_DOWNLOAD         1
#define REQUEST_SWARM_AND_HASHES  2
#define SWARM_AND_HASHES_RESPONSE 3
#define REQUEST_SWARM             4
#define SWARM_RESPONSE            5
#define CLOSE_EVERYTHING          6
#define DONE_DOWNLOADING          7
#define REQUEST_HASH              10

// Mesaje de ACK de la uploader
#define OK_DOWNLOAD     900
#define NO_DOWNLOAD     901

// Tag-uri pentru mesaje pentru a nu se incurca thread-urile de
// UP/DOWNload intre ele
#define TAG_DOWNLOAD 200
#define TAG_UPLOAD   300


struct PeerDownloadData {
    std::vector<std::string> wantedFileNames;
    std::unordered_map<std::string, std::vector<std::string>> fileHashes;
    std::unordered_map<std::string, std::vector<int>> fileSwarm;
    std::unordered_map<std::string, std::vector<std::string>> remainingHashes;
};

// Aici noteaza un peer ce vrea sa downloadeze: numele fisierelor si hash-urile
// bucatilor lipsa
PeerDownloadData downloadData;


struct PeerUploadData {
    std::unordered_map<std::string, std::vector<std::string>> ownedFileChunks;
};

// Aici tine minte un peer ce fisiere detine, precum si hash-urile sale
PeerUploadData uploadData;

// Structura pentru mentinerea datelor despre un fisier in tracker: hash-uri si swarm
struct FileInfo {
    int numSegments;
    std::vector<std::string> hashes;
    std::set<int> swarm;
};

// "Baza de date" a tracker-ului
static std::unordered_map<std::string, FileInfo> fileRegistryTracker;

void tracker(int numtasks, int rank);
void peer(int numtasks, int rank);

void* download_thread_func(void* arg);
void* upload_thread_func(void* arg);

bool tryDownloadHashFromPeer(const std::string &hashVal, int chosenPeer, int myRank);
void requestSwarmUpdate(int myRank, const std::string &fileName);
void printOwnedFiles(int rank);


int main(int argc, char* argv[])
{
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI does not support full multi-threading\n");
        exit(-1);
    }

    int numtasks, rank;
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
    return 0;
}

void tracker(int numtasks, int rank)
{

    // Primim datele de la fiecare peer
    for (int p = 1; p < numtasks; p++) {
        // Asteptam pornirea peer-ului
        int begin_communication;
        MPI_Recv(&begin_communication, 1, MPI_INT, p, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        
        // Primim datele de la el
        int numFilesOwned;
        MPI_Recv(&numFilesOwned, 1, MPI_INT, p, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        
        
        for (int i = 0; i < numFilesOwned; i++) {
            char fileNameBuf[MAX_FILENAME] = {0};
            MPI_Recv(fileNameBuf, MAX_FILENAME, MPI_CHAR, p, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            std::string fileName(fileNameBuf);

            int numSegments;
            MPI_Recv(&numSegments, 1, MPI_INT, p, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            std::vector<std::string> segs(numSegments);
            for (int s = 0; s < numSegments; s++) {
                char hashBuf[HASH_SIZE+1] = {0};
                MPI_Recv(hashBuf, HASH_SIZE, MPI_CHAR, p, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                segs[s] = std::string(hashBuf);
            }

            auto it = fileRegistryTracker.find(fileName);
            if (it == fileRegistryTracker.end()) {
                // Nu stiam de fisier pana acum, adaugam datele sale in "Database"
                FileInfo fi;
                fi.numSegments = numSegments;
                fi.hashes = segs;
                fi.swarm.insert(p);
                fileRegistryTracker[fileName] = fi;
            } else {
                // Deja stim de acest fisier, doar adaugam peer-ul in swarm
                it->second.swarm.insert(p);
            }
        }
    }

    // Trimitem READY_TO_DOWNLOAD la peer-i
    for (int p = 1; p < numtasks; p++) {
        int rtd = READY_TO_DOWNLOAD;
        MPI_Send(&rtd, 1, MPI_INT, p, 0, MPI_COMM_WORLD);
    }

    int done_downloading[MAX_PEERS] = {0};

    // Acum asteptam cereri de la peer-i, sau mesaj de DONE_DOWNLOADING
    while (true) {
        MPI_Status status;
        int msgType;
        MPI_Recv(&msgType, 1, MPI_INT, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);
        int source = status.MPI_SOURCE;

        if (msgType == REQUEST_SWARM_AND_HASHES) {
            int fnLen;
            MPI_Recv(&fnLen, 1, MPI_INT, source, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            std::vector<char> fnBuf(fnLen + 1, '\0');
            MPI_Recv(fnBuf.data(), fnLen, MPI_CHAR, source, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            std::string fileName(fnBuf.data(), fnLen);

            auto it = fileRegistryTracker.find(fileName);
            if (it == fileRegistryTracker.end()) {
                // Nu ar trebui sa ajungem aici... but just in case
                int respType = SWARM_AND_HASHES_RESPONSE;
                MPI_Send(&respType, 1, MPI_INT, source, 0, MPI_COMM_WORLD);
                int zero = 0;
                MPI_Send(&zero, 1, MPI_INT, source, 0, MPI_COMM_WORLD);
            } else {
                FileInfo &fi = it->second;
                int respType = SWARM_AND_HASHES_RESPONSE;
                MPI_Send(&respType, 1, MPI_INT, source, 0, MPI_COMM_WORLD);

                MPI_Send(&fi.numSegments, 1, MPI_INT, source, 0, MPI_COMM_WORLD);
                for (auto &h : fi.hashes) {
                    char hashBuf[HASH_SIZE + 1] = {0};
                    strncpy(hashBuf, h.c_str(), HASH_SIZE);
                    MPI_Send(hashBuf, HASH_SIZE, MPI_CHAR, source, 0, MPI_COMM_WORLD);
                }
                // Bagam si peer-ul acesta in swarm
                fi.swarm.insert(source);

                int swarmSize = (int)fi.swarm.size();
                MPI_Send(&swarmSize, 1, MPI_INT, source, 0, MPI_COMM_WORLD);
                for (int ow : fi.swarm) {
                    MPI_Send(&ow, 1, MPI_INT, source, 0, MPI_COMM_WORLD);
                }
            }
        }
        else if (msgType == REQUEST_SWARM) {
            int fnLen;
            MPI_Recv(&fnLen, 1, MPI_INT, source, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            std::vector<char> fnBuf(fnLen + 1, '\0');
            MPI_Recv(fnBuf.data(), fnLen, MPI_CHAR, source, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            std::string fileName(fnBuf.data(), fnLen);

            auto it = fileRegistryTracker.find(fileName);
            if (it == fileRegistryTracker.end()) {
                int respType = SWARM_RESPONSE;
                MPI_Send(&respType, 1, MPI_INT, source, 0, MPI_COMM_WORLD);
                int zero = 0;
                MPI_Send(&zero, 1, MPI_INT, source, 0, MPI_COMM_WORLD);
            } else {
                // !! E doar o cerere de swarm, peer-ul e deja in swarm,
                // deci nu-l mai bagam
                FileInfo &fi = it->second;
                int respType = SWARM_RESPONSE;
                MPI_Send(&respType, 1, MPI_INT, source, 0, MPI_COMM_WORLD);

                int swarmSize = (int)fi.swarm.size();
                MPI_Send(&swarmSize, 1, MPI_INT, source, 0, MPI_COMM_WORLD);
                for (int ow : fi.swarm) {
                    MPI_Send(&ow, 1, MPI_INT, source, 0, MPI_COMM_WORLD);
                }
            }
        }
        else if (msgType == DONE_DOWNLOADING) {
            done_downloading[source] = 1;
            // Verificam daca au terminat toti
            int all_done = 1;
            for (int i = 1; i < numtasks; i++) {
                if (done_downloading[i] == 0) {
                    all_done = 0;
                    break;
                }
            }
            if (all_done) {
                // Au terminat toti peer-ii, dam semnalul de inchidere totala
                for (int i = 1; i < numtasks; i++) {
                    int ce = CLOSE_EVERYTHING;
                    MPI_Send(&ce, 1, MPI_INT, i, TAG_DOWNLOAD, MPI_COMM_WORLD);
                }
                break;
            }
        }
        else {
            // Nu ar trebui sa ajung aici...
            continue;
        }
    }

}


void peer(int numtasks, int rank)
{
    std::string fname = "in" + std::to_string(rank) + ".txt";
    std::ifstream fin(fname);
    if (!fin.is_open()) {
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    // Spunem tracker-ului ca vrem sa incepem comunicarea
    int begin_communication = BEGIN_COMMUNICATION;
    MPI_Send(&begin_communication, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);
    
    
    // Trimitem, pe rand:
    // 1) nr. fisierelor obtinute
    int numFilesOwned;
    fin >> numFilesOwned;
    MPI_Send(&numFilesOwned, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

    PeerUploadData &ud = uploadData;

    // 2) numele fiecarui fisier detinut si hash-urile bucatilor
    for (int i = 0; i < numFilesOwned; i++) {
        std::string fileName;
        fin >> fileName;

        char fileNameBuf[MAX_FILENAME] = {0};
        strncpy(fileNameBuf, fileName.c_str(), MAX_FILENAME - 1);
        MPI_Send(fileNameBuf, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);

        int numSegments;
        fin >> numSegments;
        MPI_Send(&numSegments, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

        for (int s = 0; s < numSegments; s++) {
            char hashBuf[HASH_SIZE + 1] = {0};
            fin >> hashBuf; 
            ud.ownedFileChunks[fileName].push_back(std::string(hashBuf));

            // also send to tracker
            MPI_Send(hashBuf, HASH_SIZE, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);
        }
    }
    
    
    // Citesc ce fisiere doresc
    int numWanted;
    fin >> numWanted;

    PeerDownloadData &dd = downloadData;
    dd.wantedFileNames.resize(numWanted);

    for (int w = 0; w < numWanted; w++) {
        std::string fileName;
        fin >> fileName;
        dd.wantedFileNames[w] = fileName;
    }
    fin.close();

    // Astept semnalul READY_TO_DOWNLOAD
    int rtd;
    MPI_Recv(&rtd, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);


    // Obtin hash-urile fisierelor dorite si swarm-urile lor
    for (auto &fn : dd.wantedFileNames) {
        int msgType = REQUEST_SWARM_AND_HASHES;
        MPI_Send(&msgType, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

        int fnLen = (int)fn.size();
        MPI_Send(&fnLen, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);
        MPI_Send(fn.c_str(), fnLen, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);

        int responseType;
        MPI_Recv(&responseType, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        if (responseType != SWARM_AND_HASHES_RESPONSE) {
            // Nu ar trebui sa ajung aici...
            continue;
        }

        int segCount;
        MPI_Recv(&segCount, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        std::vector<std::string> segList(segCount);
        for (int i = 0; i < segCount; i++) {
            char buf[HASH_SIZE + 1] = {0};
            MPI_Recv(buf, HASH_SIZE, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            segList[i] = std::string(buf);
        }

        int swarmSize;
        MPI_Recv(&swarmSize, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        std::vector<int> swarm(swarmSize);
        for (int i = 0; i < swarmSize; i++) {
            int ow;
            MPI_Recv(&ow, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            swarm[i] = ow;
        }

        dd.fileHashes[fn] = segList;
        dd.fileSwarm[fn]  = swarm;

        std::vector<std::string> needed;  
        needed.reserve(segCount);
        for (auto &hashVal : segList) {
            needed.push_back(hashVal);
        }
        dd.remainingHashes[fn] = needed;
    }

    // Pornim thread-urile
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }

    printOwnedFiles(rank);
}

bool tryDownloadHashFromPeer(const std::string &hashVal, int chosenPeer, int myRank)
{
    int msgType = REQUEST_HASH;
    MPI_Send(&msgType, 1, MPI_INT, chosenPeer, TAG_DOWNLOAD, MPI_COMM_WORLD);

    char buf[HASH_SIZE + 1] = {0};
    strncpy(buf, hashVal.c_str(), HASH_SIZE);
    MPI_Send(buf, HASH_SIZE, MPI_CHAR, chosenPeer, TAG_DOWNLOAD, MPI_COMM_WORLD);

    int code;
    MPI_Status status;
    MPI_Recv(&code, 1, MPI_INT, chosenPeer, TAG_UPLOAD, MPI_COMM_WORLD, &status);

    if (code == OK_DOWNLOAD) {
        // Peer-ul contactat are bucata
        // In teorie, aici ar fi partea de download efectiva, si s-ar verifica
        // daca hash-ul bucatii primite corespunde cu hash-ul de la tracker
        return true;
    }
    // Nu are bucata
    return false;
}

// Functie prin care un peer cere un DOAR swarm-ul unui fisier
// (pentru cand a downloadat 10 bucati)
void requestSwarmUpdate(int myRank, const std::string &fileName)
{
    int msgType = REQUEST_SWARM;
    MPI_Send(&msgType, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

    int fnLen = (int)fileName.size();
    MPI_Send(&fnLen, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);
    MPI_Send(fileName.c_str(), fnLen, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);

    int respType;
    MPI_Recv(&respType, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    if (respType != SWARM_RESPONSE) {
        return;
    }

    int swarmSize;
    MPI_Recv(&swarmSize, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    std::vector<int> newSwarm(swarmSize);
    for (int i = 0; i < swarmSize; i++) {
        int ow;
        MPI_Recv(&ow, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        newSwarm[i] = ow;
    }

    downloadData.fileSwarm[fileName] = newSwarm;
}


void* download_thread_func(void* arg)
{
    int myRank = *(int*)arg;
    auto &dd = downloadData;
    int downloaded_chunks_from[MAX_PEERS];
    for (int i = 0; i < MAX_PEERS; i++) {
        downloaded_chunks_from[i] = 0;
    }
    int downloaded = 0;

    for (auto &fileName : dd.wantedFileNames) {
        
        auto &hashesNeeded = dd.remainingHashes[fileName];
        auto &swarm = dd.fileSwarm[fileName];

        // Downloadam bucatile de fisier pe rand
        while (!hashesNeeded.empty()) {
            if (downloaded % 10 == 0 && downloaded != 0) {
                // Facem update-ul swarm-ului odata la 10 download-uri
                requestSwarmUpdate(myRank, fileName);
                swarm = dd.fileSwarm[fileName];
            }
            
            // Am stocat hash-urile bucatilor in ordine inversa, deci luam de la capat
            std::string neededHash = hashesNeeded.back();
            hashesNeeded.pop_back();

            // Sortam swarm-ul dupa nr. de upload-uri al fiecarui peer
            std::sort(swarm.begin(), swarm.end(), 
                [&downloaded_chunks_from](int a, int b){
                    return downloaded_chunks_from[a] < downloaded_chunks_from[b];
                }
            );

            bool gotIt = false;
            for (int peerCandidate : swarm) {
                if (peerCandidate == myRank) 
                    continue;
                if (tryDownloadHashFromPeer(neededHash, peerCandidate, myRank)) {
                    // "Am downloadat bucata", notez ca o detin si cresc nr. de download-uri
                    // de la peer-ul respectiv
                    downloaded_chunks_from[peerCandidate]++;
                    downloaded++;
                    uploadData.ownedFileChunks[fileName].push_back(neededHash);
                    gotIt = true;
                    break;
                }
            }
            if (!gotIt) {
                // nu ar trebui ajuns aici...
                continue;
            }
        }
    }

    // Anuntam tracker-ului ca am terminat
    int done = DONE_DOWNLOADING;
    MPI_Send(&done, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

    return NULL;
}

void* upload_thread_func(void* arg)
{
    int myRank = *(int*)arg;
    auto &ud = uploadData;

    while (true) {
        MPI_Status status;
        int msgType;
        MPI_Recv(&msgType, 1, MPI_INT, MPI_ANY_SOURCE, TAG_DOWNLOAD, MPI_COMM_WORLD, &status);
        int sender = status.MPI_SOURCE;

        if (msgType == REQUEST_HASH) {
            // Cerere de bucata de la un peer
            char hashBuf[HASH_SIZE + 1] = {0};
            MPI_Recv(hashBuf, HASH_SIZE, MPI_CHAR, sender, TAG_DOWNLOAD, MPI_COMM_WORLD, &status);
            std::string requestedHash(hashBuf, HASH_SIZE);

            bool found = false;
            for (auto &kv : ud.ownedFileChunks) {
                const auto &chunkVec = kv.second;
                if (std::find(chunkVec.begin(), chunkVec.end(), requestedHash) != chunkVec.end()) {
                    found = true;
                    break;
                }
            }

            int code = (found ? OK_DOWNLOAD : NO_DOWNLOAD);
            MPI_Send(&code, 1, MPI_INT, sender, TAG_UPLOAD, MPI_COMM_WORLD);
        }
        else if (msgType == CLOSE_EVERYTHING) {
            // Mesaj de inchidere totala de la tracker
            break;
        }
    }

    // Inchidem thread-ul din moment ce toti au terminat de downloadat
    return NULL;
}

// Functie pentru printarea hash-urilor in fisiere
void printOwnedFiles(int rank)
{

    for (auto &file : downloadData.wantedFileNames) {
        std::string outFileName = "client" + std::to_string(rank) + "_" + file;
        std::ofstream fout(outFileName);

        auto &chunks = uploadData.ownedFileChunks[file];
        // Le-am stocat invers, deci le rastorn
        std::reverse(chunks.begin(), chunks.end());
        for (auto &hash : chunks) {
            fout << hash << "\n";
        }
        fout.close();
    
    }
    
}
