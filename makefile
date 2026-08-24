include makemacs

.PHONY: all logger server client clean

all:
	$(MAKE) -C $(LOGGER_DIR)
	$(MAKE) -C $(SERVER_DIR)
	$(MAKE) -C $(CLIENT_DIR)

logger:
	$(MAKE) -C $(LOGGER_DIR)

server: logger
	$(MAKE) -C $(SERVER_DIR) server

client: logger
	$(MAKE) -C $(CLIENT_DIR) client

clean:
	$(MAKE) -C $(LOGGER_DIR) clean
	$(MAKE) -C $(SERVER_DIR) clean
	$(MAKE) -C $(CLIENT_DIR) clean