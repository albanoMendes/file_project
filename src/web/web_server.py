#!/usr/bin/env python3

import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def list_files(directory):
    """
    Lê o diretório compartilhado com o receiver e produz a lista da API.

    Regras:
    - arquivo final normal: disponivel;
    - arquivo.part ou arquivo.meta: em_transferencia;
    - .part/.meta não aparecem como nomes técnicos na resposta:
      "video.mp4.part" vira "video.mp4".
    """
    try:
        names = sorted(os.listdir(directory))
    except OSError as error:
        raise RuntimeError(
            f"Não foi possível ler o diretório '{directory}': {error}"
        ) from error

    entries = {}

    for name in names:
        full_path = os.path.join(directory, name)

        if not os.path.isfile(full_path):
            continue

        if name.endswith(".part"):
            original_name = name[:-5]
            entries[original_name] = "em_transferencia"
            continue

        if name.endswith(".meta"):
            original_name = name[:-5]
            entries[original_name] = "em_transferencia"
            continue

        """
        Um arquivo normal representa upload concluído, pois o receiver só faz
        rename(.part -> arquivo_final) depois de validar tamanho e CRC32.
        """
        if name not in entries:
            entries[name] = "disponivel"

    return [
        {"name": filename, "status": entries[filename]}
        for filename in sorted(entries)
    ]


def create_handler(directory):
    """
    Cria uma classe HTTP ligada ao diretório informado na linha de comando.
    """

    class FileHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            """
            Trata GET /files.

            Outras rotas retornam HTTP 404 em JSON.
            """
            if self.path != "/files":
                body = json.dumps(
                    {"error": "Rota não encontrada"},
                    ensure_ascii=False
                ).encode("utf-8")

                self.send_response(404)
                self.send_header(
                    "Content-Type",
                    "application/json; charset=utf-8"
                )
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return

            try:
                files = list_files(directory)
                body = json.dumps(
                    files,
                    ensure_ascii=False,
                    indent=2
                ).encode("utf-8")

            except RuntimeError as error:
                body = json.dumps(
                    {"error": str(error)},
                    ensure_ascii=False
                ).encode("utf-8")

                self.send_response(500)
                self.send_header(
                    "Content-Type",
                    "application/json; charset=utf-8"
                )
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return

            self.send_response(200)
            self.send_header(
                "Content-Type",
                "application/json; charset=utf-8"
            )
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, format_string, *args):
            """
            Log simples e legível no terminal.
            """
            print(
                f"[HTTP] {self.client_address[0]} - "
                f"{format_string % args}"
            )

    return FileHandler


def main():
    """
    Uso:
        python3 src/web/web_server.py <diretorio> [porta]

    Exemplos:
        python3 src/web/web_server.py ./storage
        python3 src/web/web_server.py ./storage 8080
    """
    if len(sys.argv) not in (2, 3):
        print(
            f"Uso: {sys.argv[0]} <diretorio> [porta]",
            file=sys.stderr
        )
        sys.exit(1)

    directory = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) == 3 else 8080

    if not os.path.isdir(directory):
        print(
            f"Diretório inválido: {directory}",
            file=sys.stderr
        )
        sys.exit(1)

    handler = create_handler(directory)

    """
    ThreadingHTTPServer permite que mais de uma consulta HTTP seja atendida
    sem bloquear as demais. Ele não interfere no receiver UDP: são processos
    separados que apenas leem/escrevem o mesmo diretório.
    """
    server = ThreadingHTTPServer(("0.0.0.0", port), handler)

    print(f"Servidor web em http://0.0.0.0:{port}")
    print("Endpoint disponível: GET /files")
    print(f"Diretório monitorado: {directory}")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nEncerrando servidor web...")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()