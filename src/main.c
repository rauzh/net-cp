#include "server.h"
#include <stdlib.h>

int main(void) {

    struct settings server_settings = {INADDR_ANY, PORT, 2, 16384};

    struct server* srv = server_init(&server_settings);

    server_serve(srv);

    server_shutdown(srv);

    return EXIT_SUCCESS;
}
