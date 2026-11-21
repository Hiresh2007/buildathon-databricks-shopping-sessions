FROM python:3.12-slim-bookworm AS runtime

FROM runtime AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY sessions.cpp input.hpp identity.hpp parquet_output.hpp build.py ./
RUN python build.py

FROM runtime
WORKDIR /app
COPY --from=build /src/sessions .
COPY run.py .
ENV PYTHONDONTWRITEBYTECODE=1
USER 1000:1000
CMD ["/app/sessions"]
