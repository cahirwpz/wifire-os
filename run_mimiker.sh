docker build . -t mimiker-dev:latest
docker-compose up -d
docker-compose exec mimiker make
docker-compose exec mimiker tmux

