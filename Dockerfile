FROM gcc:12-bookworm AS build
WORKDIR /src
COPY sessions.cpp input.hpp ./
RUN g++ -std=c++17 -O3 -pthread -static-libstdc++ -static-libgcc sessions.cpp -o /sessions

FROM python:3.12-slim-bookworm
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY --from=build /sessions .
COPY run.py .
ENV PYTHONDONTWRITEBYTECODE=1 SCRATCH_DIR=/scratch
USER 1000:1000
CMD ["python", "/app/run.py"]
