# The Metin2 Deployment Files
The Old Metin2 Project aims at improving and maintaining the 2014 Metin2 game files up to modern standards. The goal is to archive the game as it was in order to preserve it for the future and enable nostalgic players to have a good time.

For-profit usage of this material is certainly illegal without the proper licensing agreements and is hereby discouraged (not legal advice). Even so, the nature of this project is HIGHLY EXPERIMENTAL - bugs are to be expected for now.

## Usage
The deployment strategy for this project is based around Docker Compose. For now, no images are published, so building your own server image is required. Also, for now, you need to provide your own database schema.

### Building the server image
This process requires that you do the following steps on a Linux environment with Docker installed and running.

Clone the Server project repository:
```shell
git clone https://git.old-metin2.com/metin2/server.git
```

Build the image:
```shell
cd server
docker build -t metin2/server:test .
```

### Starting the server
Clone this repository and open a terminal window in its root directory. Then, simply bring up the Compose project:
```shell
docker compose up -d
```

On the first run, you might need to connect to port 3306 with your favourite MySQL client (Navicat, DBeaver, phpMyAdmin etc.) and install a Metin2 database schema.
