
import socket
import ssl

HOST = "ip-dev.itspersonalservices.com"  # The server's hostname or IP address

context = ssl._create_unverified_context()

with socket.create_connection((HOST, 443)) as sock:
    with context.wrap_socket(sock, server_hostname=HOST) as ssock:
        print(ssock.version())

