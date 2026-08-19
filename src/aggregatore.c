#include <sys/socket.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/file.h>   
#include <netinet/in.h> 
#include <arpa/inet.h>   
#include <stdint.h>
#include <time.h>


typedef struct {
    int32_t  timestamp;
    uint32_t uid;
    int32_t  dato;
} MESSAGE;

int const MAX_QUEUED = 100;
// la dimensione massima consentita per il file
int const MAX_DIMENSION = 2000000;
// Flag globale per fermare la routine ( più duttile di un while true )
int routineFlag = 0;


struct sockaddr_in initialize_socket(struct in_addr indirizzo, in_port_t porta, int *socket_fd_pointer);    
int read_message(void* buffer, int file_descriptor, int byte_mancanti);
void gestisci_sigpipe(int segnale);
void gestisci_sigint(int segnale);
void manage_sigalarm(int segnale);
/*
Il coordinatore dovrà opportunamente gestire i seguenti segnali
- SIGPIPE, inviato da un produttore quando  chiude la connessione,
 deve essere gestito da un handler che scrive nel file di log 
 [TIMESTAMP, ID_MITTENTE, "DISCONNECT"]
- SIGINT: il coordinatore deve eseguire una terminazione controllata,
 ovvero un handler che chiuda tutti i socket in stato LISTENING,
  attenda che tutti i processi/thread abbiano terminato la scrittura e,
   infine, chiuda il file di log.

Il Coordinatore, mediante l'uso di SIGALRM,
dovrà periodicamente verificare che il file di log 
non superi una dimensione massima prefissata e, 
qualora la dimensione massima sia stata raggiunta, 
dovrà creare un nuovo file di log, archiviando il precedente. // banalmente qui basta rinominare il vecchio e riaprire il nuovo col classico nome

*/

// Questa funzione si occupa di controllare la dimensione del file di log e in caso di creare un nuovo log
void manage_sigalarm(int sig) {
    if (sig == SIGALRM) {
        // devo verificare la dimensione del file di log
        if (file != NULL) {
            struct stat stats;
            if (stat("log.txt", &stats) != 0){
                perror("Errore stats");
                exit(-1);
                // Archivio il file e ne creo uno nuovo con lo stesso nome
                // dell'originale
                time_t timestampCorrente = time(NULL);
                struct tm* tempoLocale = localtime(&timestampCorrente);
                char timeStamp[20];
                strftime(timeStamp, sizeof(timeStamp), "%Y%m%d_%H%M%S", tempoLocale);
                char nomeLog[64];
                snprintf(nomeLog, sizeof(nomeLog), "log_%s.txt", timeStamp);
                if (rename("log.txt", nomeLog) == 0) {
                    printf("File di log archiviato...");
                    FILE *newFile = fopen("log.txt", "w");
                    printf("Creazione di un nuovo log...");
                    fclose(newFile);
                }
                else {
                    printf("errore durante l'archiviazione del log...");
                }
            }
        }
    }
    // Al termine, se la flag non è stata bloccata, reimposta un timer che richiamerà questo metodo a tempo debito
    if (routineFlag == 1) {
        alarm(1);
    }
}

int main(int argc, char* args[]){
    if (argc < 2){
        printf("Inserire in input PORTA")
    }
    // Dopo aver indicato la funzione deputata a gestire il SIGALARM, setto la condizione del loop
    // ed inizializzo il primo alarm che darà avvio alla routine ciclica
    signal (SIGALRM, manage_sigalarm);
    routineFlag = 1;
    alarm(1);

    //creo socket
    int sfd; // server file descriptor
    char *endptr; // puntatore per controllare argomenti
    errno = 0; // setto errno a zero
    long porta = strtol(args[1], &endptr, 10);   // leggo la porta in base 10
    // se ci sono stati errori allora è colpa della porta
    if (errno != 0 || *endptr != '\0') {
        perror("Porta di formato non valido");
        exit(1);
    }
    if (porta < 1 || porta > 65535) {
       fprintf(stderr, "Porta non valida\n");
       exit(1);
   }
    struct in_addr indirizzo;
    indirizzo.s_addr = htonl(INADDR_LOOPBACK); //ip su loopback 127.0.0.1 
    in_port_t porta_rete = htons((uint16_t) porta);
    struct sockaddr_in addr = initialize_socket(indirizzo, porta_rete, &sfd);
    // finito di creare la socket


    // disabilito sigchld
    struct sigaction sa; // inizio a disabilitare sigchld
    memset(&sa, 0, sizeof(sa)); // svuoto sigaction
    sa.sa_handler = SIG_DFL; // si comporta di default quando lo vede
    sa.sa_flags = SA_NOCLDWAIT;// non crea zombie quando i figli terminano
    sigaction(SIGCHLD, &sa, NULL);//imposto 
    //fine disabilitazione sigchld
    while(1){
        socklen_t addrlen = sizeof(struct sockaddr);
        int client_sd = accept(sfd, (struct sockaddr *) &addr, &addrlen); // accetto un client
        if(client_sd == -1){ 
            if (errno != EINTR)  perror("Errore accettando client"); //gestisco l'errore dovuto a interrupt di sigalarm
            continue;
        }
        if (fork() == 0){
            close(sfd); // al figlio questo non serve più
            MESSAGE msg; // allochiamo un buffer
            while(read_message((void *)&msg, client_sd, sizeof(MESSAGE)) == 0){ // gestiamo la lettura
                int32_t  timestamp = (int32_t) ntohl((uint32_t) msg.timestamp);
                uint32_t uid = ntohl(msg.uid);
                int32_t  dato = (int32_t) ntohl((uint32_t) msg.dato);
                FILE *log_file = fopen("log.txt", "a"); //apro il log file
                int file_descriptor = fileno(log_file); //prendo il file descriptor
                flock(file_descriptor, LOCK_EX); //faccio il lock
                fprintf(log_file, "[%d, %u, %d]\n", timestamp, uid, dato); //scrivo su file
                fflush(log_file); //faccio flush per svuotare il buffer di fprintf
                flock(file_descriptor, LOCK_UN); //rilascio il lock
                fclose(log_file); //chiudo il file
            }
            exit(0); // il figlio non deve ricominciare il loop del padre
        }
    }
}


int read_message(void* buffer, int file_descriptor, int byte_mancanti){
    int byte_letti = 0;
    int r;
    while(byte_letti < byte_mancanti){
        r = read(file_descriptor, (char*)buffer + byte_letti, byte_mancanti - byte_letti);
        if (r <= 0) return -1;  
        byte_letti += r;
    }
    return 0;
}

struct sockaddr_in initialize_socket(struct in_addr indirizzo, in_port_t porta, int *socket_fd_pointer) {

    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if (sd == -1) {
        perror("Errore creazione socket");
        exit(-1);
    }

    int opt = 1;
    if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {//utilizzo reuseaddr
        perror("Errore settaggio reuseaddr");  // se c'è errore usciamo 
        exit(-1);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr)); // azzeriamo addr e configuriamo per la connessione TCP
    addr.sin_family = AF_INET; // tcp
    addr.sin_port = porta; // porta
    addr.sin_addr = indirizzo; // ip

    if (bind(sd, (struct sockaddr*)&addr, sizeof(addr)) == -1) { // gestiamo errrori
        perror("Errore socket bind");
        exit(-1);
    }

    if (listen(sd, MAX_QUEUED) == -1) { //gestione errori
        perror("Errore socket listen");
        exit(-1);
    }

    *socket_fd_pointer = sd;   // scrivo il fd nel puntatore passato dal chiamante

    return addr;
}