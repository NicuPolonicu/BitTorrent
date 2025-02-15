# BitTorrent Protocol - Tema 2 APD

**Autor:** Nicula Dan Alexandru - 332CA



## Detalii Implementare
Această implementare include cele două componente principale ale protocolului **BitTorrent**:
- **Tracker**
- **Peer**

### Peer
Peer-ul funcționează în următorii pași:

1. **Citirea fișierului de intrare**
   - Se citesc fișierele deținute și cele dorite.
   
2. **Comunicare inițială cu Tracker-ul**
   - Peer-ul trimite tracker-ului lista fișierelor pe care le deține, inclusiv hash-urile acestora.
   
3. **Așteptare semnal READY_TO_DOWNLOAD**
   - Peer-ul așteaptă semnalul `READY_TO_DOWNLOAD` de la tracker.
   
4. **Solicitarea hash-urilor și swarm-urilor pentru fișierele dorite**
   - Peer-ul trimite către tracker un mesaj `REQUEST_SWARM_AND_HASHES` pentru fiecare fișier dorit.
   
5. **Thread de Download**
   - Peer-ul descarcă **pe rând** fișierele dorite, respectând următoarea strategie:
     - Parcurge secvențial hash-urile fișierelor dorite.
     - Trimite cereri către membrii swarm-ului pentru fiecare hash.
     - **Sortează swarm-ul crescător** după numărul de descărcări efectuate anterior de la fiecare peer, pentru a menține echilibrată distribuția descărcărilor.
     - După ce toate bucățile unui fișier sunt descărcate, trece la următorul fișier.
   - Când toate fișierele dorite sunt descărcate, peer-ul trimite `DONE_DOWNLOADING` către tracker și închide acest thread.

6. **Thread de Upload**
   - Peer-ul așteaptă mesaje de tip `TAG_DOWNLOAD` de la alți peer-i (solicitări de download) sau mesajul `CLOSE_EVERYTHING` de la tracker.
   - Dacă mesajul este `CLOSE_EVERYTHING`, thread-ul se oprește.
   - În cazul unei cereri de download:
     - Verifică dacă deține bucata de fișier solicitată.
     - Trimite `OK_DOWNLOAD` dacă o are, sau `NO_DOWNLOAD` dacă nu.
     - Reia așteptarea altor mesaje.

7. **Închiderea Peer-ului**
   - După închiderea thread-urilor de upload și download, peer-ul își termină execuția.

---

### Tracker
Tracker-ul gestionează fișierele și swarm-urile asociate. Implementarea acestuia este următoarea:

1. **Recepționarea informațiilor inițiale de la peer-i**
   - Tracker-ul primește lista fișierelor deținute de fiecare peer, inclusiv hash-urile acestora.
   - Construiește o bază de date cu toate fișierele disponibile și swarm-urile asociate.

2. **Trimiterea semnalului READY_TO_DOWNLOAD**
   - După ce a primit toate datele inițiale de la peer-i, trimite semnalul `READY_TO_DOWNLOAD` către toți peer-ii.

3. **Gestionarea cererilor primite**
   - Așteaptă mesaje din partea peer-ilor, care pot fi:
     - **`REQUEST_SWARM_AND_HASHES`** → Peer-ul cere hash-urile și swarm-ul unui fișier.
       - Tracker-ul răspunde cu informațiile cerute și adaugă peer-ul în swarm.
     - **`REQUEST_SWARM`** → Peer-ul vrea doar swarm-ul (fără hash-uri, deoarece le are deja).
       - Tracker-ul răspunde fără a adăuga peer-ul în swarm.
     - **`DONE_DOWNLOADING`** → Peer-ul anunță că a terminat descărcarea fișierelor dorite.
       - Tracker-ul marchează peer-ul ca „terminat”.

4. **Închiderea Tracker-ului**
   - După ce **toți** peer-ii au trimis `DONE_DOWNLOADING`, tracker-ul:
     - Trimite semnalul `CLOSE_EVERYTHING` către toți peer-ii.
     - Se închide automat.
