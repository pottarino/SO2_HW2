#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

typedef struct {
    int32_t  timestamp;
    uint32_t uid;
    int32_t  dato;
} MESSAGE;

int main(int argc, char *argv[]) {

    if (argc < 4) {
        fprintf(stderr, "Uso: %s <ip> <porta> <uid>\n", argv[0]);
        exit(1);
    }

    char *endptr;
    errno = 0;
    long porta = strtol(argv[2], &endptr, 10);
    if (errno != 0 || *endptr != '\0' || porta < 1 || porta > 65535) {
        fprintf(stderr, "Porta non valida\n");
        exit(1);
    }

    errno = 0;
    long uid = strtol(argv[3], &endptr, 10);
    if (errno != 0 || *endptr != '\0') {
        fprintf(stderr, "UID non valido\n");
        exit(1);
    }

    // creo il socket (il client NON fa bind/listen, solo connect)
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd == -1) {
        perror("Errore creazione socket");
        exit(1);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) porta);

    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1) {
        fprintf(stderr, "Indirizzo IP non valido\n");
        exit(1);
    }

    if (connect(cfd, (struct sockaddr*) &addr, sizeof(addr)) == -1) {
        perror("Errore connessione al server");
        exit(1);
    }

    printf("Connesso al server. Invio messaggi (Ctrl+C per uscire)...\n");

    // invio un messaggio ogni secondo, con un valore casuale come "dato"
    srand(time(NULL) ^ getpid());

    while (1) {
        MESSAGE msg;
        msg.timestamp = htonl((int32_t) time(NULL));
        msg.uid       = htonl((uint32_t) uid);
        msg.dato      = htonl((int32_t) (rand() % 1000));

        if (write(cfd, &msg, sizeof(msg)) != sizeof(msg)) {
            perror("Errore invio messaggio");
            break;
        }

        printf("Messaggio inviato\n");
        sleep(1);
    }

    close(cfd);
    return 0;
}