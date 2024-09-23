# nanoHTTP
A nano lightweight, educational HTTP server implementation in C. Ideal for learning network programming basics and understanding HTTP server operations. Features simple GET and POST request handling in a single-file design.

## Run

1. Compile:
   ```
   gcc nanohttp.c -o nanohttp
   ```

2. Execute:
   ```
   ./nanohttp
   ```

3. Server runs on port 8080.

## Test

GET request:
```
curl http://localhost:8080
```

POST request:
```
curl -X POST -d "Hello" http://localhost:8080
```
