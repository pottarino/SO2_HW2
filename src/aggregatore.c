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
#include <sys/stat.h>
#include <sys/wait.h>


typedef struct {
    int32_t  timestamp;
    uint32_t uid;
    int32_t  dato;
} MESSAGE;
// qui ci metto i process id dei children per poter fare la wait sulla exit
int children_size = 100;
pid_t *children;
int children_number = 0;
int const MAX_QUEUED = 100;
// la dimensione massima consentita per il file
int const MAX_DIMENSION = 2000000;
// Flag globale per fermare la routine ( più duttile di un while true )
int routineFlag = 0;
// Id del processo che è associato a ciascun child
uint32_t id_processo = 0;
// Flag per la exit controllata con SIGINT (volatile per essere riletta ogni ciclo,
// atomica perchè condivisa tra
// signal handler e codice normale)
volatile  sig_atomic_t exit_queued = 0;


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
    // La funzione non è accessibile ai children perchè non chiamano alarm
    if (sig == SIGALRM) {
        // devo verificare la dimensione del file di log
        FILE *filelog = fopen("log.txt", "a");
        if (filelog != NULL) {
            fclose(filelog);
            struct stat stats;
            if (stat("log.txt", &stats) != 0) {
                perror("Errore stats");
                exit(-1);
            }
            // Archivio il file e ne creo uno nuovo con lo stesso nome
            // dell'originale
            if (stats.st_size >= MAX_DIMENSION) {
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
        // Al termine, se la flag non è stata bloccata, reimposta un timer che richiamerà questo metodo a tempo debito
        if (routineFlag == 1) {
            alarm(1);
        }
    }
}



// Annotazione: fopen fprintf, localtime, etc... non sono "safe" nel sistema asincrono. Ma la traccia
// richiede esplicitamente che l'handler scriva sui logs, quindi ignoriamo questo warning.
void manage_sigpipe(int sig) {
    // l'UID associato a questo child è nel campo id_processo
    // prendo i locks sul file di log
    FILE *log_file = fopen("log.txt", "a"); //apro il log file
    if (log_file != NULL) {
        int file_descriptor = fileno(log_file); //prendo il file descriptor
        flock(file_descriptor, LOCK_EX); //faccio il lock
        // Prendo il timestamp
        time_t timestampCorrente = time(NULL);
        struct tm* tempoLocale = localtime(&timestampCorrente);
        char timeStamp[20];
        strftime(timeStamp, sizeof(timeStamp), "%Y%m%d_%H%M%S", tempoLocale);
        fprintf(log_file, "[%s, %u, DISCONNECT]\n", timeStamp, id_processo); //scrivo su file
        fflush(log_file); //faccio flush per svuotare il buffer di fprintf
        flock(file_descriptor, LOCK_UN); //rilascio il lock
        fclose(log_file); //chiudo il file
        // rilascio i locks
        // ( controllare se viene killato il child in automatico)
    }
}

void manage_sigint(int sig) {
    // Chiudo la routine di SIGLARM
    routineFlag = 0;
    // Chiudo la routine che accetta nuove connessioni
    exit_queued = 1;
}

void manage_sigchld(int sig){
    int status;
    pid_t pid;
    do{
        pid = waitpid(-1, &status, WNOHANG); // aspetto qualsiasi figlio che sia zombie
        if(pid <= 0) break;
        for (int i = 0; i < children_number; i++) { // cerco il children
            if (children[i] == pid) { // quando trovo il pid
                children[i] = children[children_number - 1]; // prendo l'ultimo children e metto l'ultimo al posto di quello tolto
                children_number--; // e abbasso la lunghezza
                break;
            }
        }
    }while(pid > 0); //finchè ce ne sono
}

int main(int argc, char* args[]) {

    // Inizializzo l'allocazione dinamica della memoria per i children+
    children = malloc(sizeof(pid_t)*children_size);
    if (children == NULL) {
        printf("Errore duranta l'allocazione iniziale della memoria...");
        exit(EXIT_FAILURE);
    }

    // Inizializzazione della sig mask per poter bloccare e ripristinare SIGCHILD durante la realloc
    // Ed evitare memory leaks
    sigset_t SIGCHLD_set, old_set;
    sigemptyset(&SIGCHLD_set);
    sigaddset(&SIGCHLD_set, SIGCHLD);


    if (argc < 2){
        printf("Inserire in input PORTA");
        exit(1);
    }
    // Dopo aver indicato la funzione deputata a gestire il SIGALARM, setto la condizione del loop
    // ed inizializzo il primo alarm che darà avvio alla routine ciclica
    signal (SIGALRM, manage_sigalarm);
    routineFlag = 1;
    alarm(1);

    // Stessa cosa per SIGINT
    signal (SIGINT, manage_sigint);


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
    signal(SIGCHLD, manage_sigchld);

    while(exit_queued == 0) {

        // Qui comincia la sezione critica in cui bisogna tenere a freno SIGCHLD
        sigprocmask(SIG_BLOCK, &SIGCHLD_set, &old_set);

        // Checko la size dell'array dei children: se la dimensione va riallocata devo controllarlo qui
        if (children_number >= children_size) {
            children_size *= 2;
            pid_t *tmp_children = realloc(children, children_size*sizeof(pid_t));
            if (tmp_children == NULL) {
                printf("Errore malloc");
                // Sblocco la sezioen critica in caso di fallimento
                sigprocmask(SIG_SETMASK, &old_set, NULL);
                // Non sono riuscito a riallocare la memoria, pertanto non posso più accogliere nuove
                // Richieste; setto la flag per terminare
                exit_queued = 1;
                continue;
            }
            children = tmp_children;
        }

        // Rilascio sezione critica
        sigprocmask(SIG_SETMASK, &old_set, NULL);

        // Se è andato tutto bene...
        socklen_t addrlen = sizeof(struct sockaddr);
        int client_sd = accept(sfd, (struct sockaddr *) &addr, &addrlen); // accetto un client
        if(client_sd == -1){
            if (errno == EINTR) {
                // Interrompo esplicitamente il ciclo se la exit_flag è settata
                if (exit_queued) break;
                continue;
            }
            perror("Errore accettando client"); //gestisco l'errore dovuto a interrupt di sigalarm
            continue;

        }

        // Esplicito l'id_processo assegnato per definire se è child o meno e poterlo mettere in children
        pid_t pid = fork();
        if (pid == 0){
            // Passo il SIGPIPE alla funzione deputata
            signal (SIGPIPE, manage_sigpipe);
            close(sfd); // al figlio questo non serve più
            MESSAGE msg; // allochiamo un buffer
            while(read_message((void *)&msg, client_sd, sizeof(MESSAGE)) == 0){ // gestiamo la lettura
                int32_t  timestamp = (int32_t) ntohl((uint32_t) msg.timestamp);
                uint32_t uid = ntohl(msg.uid);
                // Setto il campo ( serve per il SIGPIPE dei children)
                id_processo = uid;
                int32_t  dato = (int32_t) ntohl((uint32_t) msg.dato);
                FILE *log_file = fopen("log.txt", "a"); //apro il log file
                int file_descriptor = fileno(log_file); //prendo il file descriptor
                flock(file_descriptor, LOCK_EX); //faccio il lock
                fprintf(log_file, "[%d, %u, %d]\n", timestamp, uid, dato); //scrivo su file
                fflush(log_file); //faccio flush per svuotare il buffer di fprintf
                flock(file_descriptor, LOCK_UN); //rilascio il lock
                fclose(log_file); //chiudo il file
            }
            // Genero la SIGPIPE
            raise(SIGPIPE);
            //close(client_sd); è ridondante perchè già viene chiusa con la exit
            exit(0); // il figlio non deve ricominciare il loop del padre
        }
        else if(pid == -1){
            perror("Errore nella creazione di un processo");
            close(client_sd);
            continue;
        }
        else{
            // Apro sezione critica per registrare il pid
            sigprocmask(SIG_BLOCK, &SIGCHLD_set, &old_set);
            // Metto il pid nella struttura dati apposita
            children[children_number++] = pid;
            // CHiudo sezione critica
            sigprocmask(SIG_SETMASK, &old_set, NULL);
        }
        close(client_sd);
    }
    // La flag per la terminazione controllata provoca la chiusura a nuove connessioni
    // si aspetta che i figli abbiano finito e notifica il successo
    // rispetto a prima se i processi terminano prima non ne impedisce la zombificazione
    if (exit_queued == 1) {
        close(sfd);
        signal(SIGCHLD, SIG_DFL); //ignoro sigchld, ora non mi serve più
        // Elimina la necessità di tracciare i singoli PID, si aspetta solo che non ci siano più figli
        // Ergo funziona anche con un array disallineato
        while(waitpid(-1, NULL, 0) > 0){}
        printf("tutti gli aggregatori sono disabilitati");

    }
    free(children);
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