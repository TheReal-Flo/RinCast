FROM golang:1.26-alpine AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /out/rincast-server .

FROM gcr.io/distroless/static-debian12
COPY --from=build /out/rincast-server /rincast-server
VOLUME /data
EXPOSE 8080/tcp 38720/udp
ENV RINCAST_DATA=/data
ENTRYPOINT ["/rincast-server"]
