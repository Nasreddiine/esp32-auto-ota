FROM espressif/idf:release-v5.1

WORKDIR /workspace

COPY scripts/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

RUN useradd -m -s /bin/bash builder
USER builder

ENTRYPOINT ["/entrypoint.sh"]