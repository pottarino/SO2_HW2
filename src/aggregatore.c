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
    uint32_t uid;
    int32_t  dato;
} MESSAGE;

int const MAX_QUEUED = 100;
// la dimensione massima consentita per il file
int const MAX_DIMENSION = 2000000;
// flag globale per fermare la routine (più duttile di un while true)
int exit_queued = 0;
// flag per la exit controllata con SIGINT 
int log_number = 0;


struct sockaddr_in initialize_socket(struct in_addr indirizzo, in_port_t porta, int *socket_fd_pointer);    
int read_message(void* buffer, int file_descriptor, int byte_mancanti);
void manage_sigpipe(int segnale, siginfo_t *info, void *other);
void manage_sigint(int segnale);
void manage_sigalrm(int segnale);
void close_connection(int sfd);
void child_behaviour(int client_sd);
void send_sigpipe(unsigned int numero);

int main(int argc, char* args[]) {

    if (argc < 2){
        printf("Inserire in input PORTA");
        exit(1);
    }

    //creo socket
    int sfd; // server file descriptor
    char *endptr; // puntatore per controllare argomenti
    errno = 0; // setto errno a zero
    long porta = strtol(args[1], &endptr, 10);   // leggo la porta in base 10
    // se ci sono stati errori allora è colpa della porta
    if (errno != 0 || *endptr != '\0') {
        perror("Porta di formato non valido\n");
        exit(1);
    }
    if (porta < 1 || porta > 65535) {
        perror("Porta non valida\n");
        exit(1);
    }
    // inizializziamo la struct per la socket
    struct in_addr indirizzo;
    indirizzo.s_addr = htonl(INADDR_LOOPBACK); //ip su loopback 127.0.0.1
    in_port_t porta_rete = htons((uint16_t) porta);
    struct sockaddr_in addr = initialize_socket(indirizzo, porta_rete, &sfd);
    // finito di creare la socket
    
    //disabilito sigchld
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = SA_NOCLDWAIT;
    sigaction(SIGCHLD, &sa, NULL);

    //gestiamo sigalarm con la funzione giusta
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = manage_sigalrm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);

    // Stessa cosa per SIGINt
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = manage_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    // Dopo aver indicato la funzione deputata a gestire il SIGALARM, 
    // inizializzo il primo alarm che darà avvio alla routine ciclica
    
    alarm(2);

    while(exit_queued == 0) {
        //accettiamo nuove connessioni finchè non viene per qualche motivo richiesta la chiusura

        socklen_t addrlen = sizeof(struct sockaddr);
        int client_sd = accept(sfd, (struct sockaddr *) &addr, &addrlen); // accetto un client
        if(client_sd == -1){
            if (errno == EINTR) { //gestisco l'errore dovuto a interrupt di sigalarm/sigint
                // Interrompo esplicitamente il ciclo se la exit_flag è settata
                if (exit_queued){
                    close_connection(sfd);
                    break;
                } 
                continue;
            }
            perror("Errore accettando client"); 
            continue;
        }
        pid_t pid = fork();
        if (pid == 0){
            close(sfd);
            child_behaviour(client_sd);
        }
        close(client_sd);
        // La flag per la terminazione controllata provoca la chiusura a nuove connessioni
        // si aspetta che i figli abbiano finito e notifica il successo
        // rispetto a prima se i processi terminano prima non ne impedisce la zombificazione
        if (exit_queued == 1) close_connection(sfd);
    }
}

// Questa funzione si occupa di controllare la dimensione del file di log e in caso di creare un nuovo log
void manage_sigalrm(int sig) {
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
                char timeStamp[40];
                strftime(timeStamp, sizeof(timeStamp), "%d_%m_%Y__%H_%M_%S", tempoLocale);
                char nomeLog[64];
                snprintf(nomeLog, sizeof(nomeLog), "log_%s_s%d.txt", timeStamp, log_number++);
                if (rename("log.txt", nomeLog) == 0) {
                    printf("\nFile di log archiviato...");
                }
                else 
                    printf("\nerrore durante l'archiviazione del log...");
                    
            }   
        }
        // Al termine, se la flag non è stata bloccata, reimposta un timer che richiamerà questo metodo a tempo debito
        if (!exit_queued) alarm(2);
    }
}

void manage_sigpipe(int sig, siginfo_t *info, void*other){
    uint32_t process_uid = (uint32_t) info->si_value.sival_int;
        FILE *log_file = fopen("log.txt", "a"); //apro il log file
    if (log_file != NULL) {
        int file_descriptor = fileno(log_file); //prendo il file descriptor
        flock(file_descriptor, LOCK_EX); //faccio il lock
        // Prendo il timestamp
        time_t timestampCorrente = time(NULL);
        struct tm* tempoLocale = localtime(&timestampCorrente);
        char timeStamp[50];
        strftime(timeStamp, sizeof(timeStamp), "%d/%m/%Y - %H:%M:%S", tempoLocale);
        fprintf(log_file, "[%s, %u, DISCONNECT]\n", timeStamp, process_uid); //scrivo su file
        fflush(log_file); //faccio flush per svuotare il buffer di fprintf
        flock(file_descriptor, LOCK_UN); //rilascio il lock
        fclose(log_file); //chiudo il file
        // rilascio i locks
        // (controllare se viene killato il child in automatico)
    }

}
void manage_sigint(int sig) {
    // Chiudo la routine che accetta nuove connessioni e gestisce SIGALARM
    exit_queued = 1;
}

void close_connection(int sfd){
        close(sfd);
        //aspetto che tutti i figli terminino
        waitpid(-1, NULL, 0);
        printf("tutti gli aggregatori sono disabilitati\n");
}

void child_behaviour(int client_sd){

    MESSAGE msg; // allochiamo un buffer per leggere un messaggio alla volta
    time_t timestampCorrente;
    struct tm *tempoLocale;
    int32_t  dato;
    int file_descriptor;
    unsigned long uid = 0;
    char timeStamp[50];
    // Gestione sigpipe 1..1 
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = manage_sigpipe;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);

    while(read_message((void *)&msg, client_sd, sizeof(MESSAGE)) == 0){ // gestiamo la lettura

        if(uid == 0) uid = ntohl(msg.uid);
        else if(uid != ntohl(msg.uid)) {
            printf("Trovato e chiuso produttore malevolo\n");
            send_sigpipe(uid);
            close(client_sd);
            exit(-1);
        }

        dato = (int32_t) ntohl((uint32_t) msg.dato);
        FILE *log_file = fopen("log.txt", "a"); //apro il log file
        file_descriptor = fileno(log_file); //prendo il file descriptor
        flock(file_descriptor, LOCK_EX); //faccio il lock
        timestampCorrente = time(NULL); // prendo il timestamp
        tempoLocale = localtime(&timestampCorrente);
        strftime(timeStamp, sizeof(timeStamp), "%d/%m/%Y - %H:%M:%S", tempoLocale);
        fprintf(log_file, "[%s, %u, %d]\n", timeStamp, uid, dato); //scrivo su file
        fflush(log_file); //faccio flush per svuotare il buffer di fprintf
        flock(file_descriptor, LOCK_UN); //rilascio il lock
        fclose(log_file); //chiudo il file
    }
    send_sigpipe(uid);
    // Genero la SIGPIPE perchè la chiamata è chiusa
    close(client_sd); //è ridondante perchè già viene chiusa con la exit
    exit(0); // il figlio non deve ricominciare il loop del padre
}

void send_sigpipe(uint32_t uid){
    union sigval s;
    s.sival_int = uid;
    sigqueue(getpid(), SIGPIPE, s); // mando a me stesso sigpipe
}

int read_message(void* buffer, int file_descriptor, int byte_mancanti){
    int byte_letti = 0;
    int r;
    while(byte_letti < byte_mancanti){
        r = read(file_descriptor, (char*) buffer + byte_letti, byte_mancanti - byte_letti);
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
    //la consegna chiede un rapido riuso della coppia ip:port
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