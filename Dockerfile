FROM python:3.12-slim-bookworm AS runtime

FROM runtime AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY sessions.cpp input.hpp identity.hpp parquet_output.hpp build.py ./
RUN python build.py && mkdir /src/bin && mv sessions /src/bin/sessions
COPY cpu_dispatch.c ./
RUN if [ "$(uname -m)" = "x86_64" ]; then \
        mv /src/bin/sessions /src/bin/sessions-generic && \
        CXXFLAGS="-O3 -march=x86-64-v3 -mtune=haswell -flto" python build.py && \
        mv sessions /src/bin/sessions-v3 && \
        cc -std=c11 -O2 -static -s cpu_dispatch.c -o /src/bin/sessions; \
    fi

FROM runtime
WORKDIR /app
COPY --from=build /src/bin/ ./
COPY run.py .
ENV PYTHONDONTWRITEBYTECODE=1
USER 1000:1000
CMD ["/app/sessions"]
