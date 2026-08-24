include makemacs

all: $(SERVER) $(CLIENT)

server: $(SERVER)

client: $(CLIENT)

test: $(SERVER) $(CLIENT) $(TEST)

$(SERVER): $(SRC)/server_main.cpp $(COMMON_OBJS) $(LOGGER_LIB)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)

$(CLIENT): $(SRC)/client_main.cpp $(BUILD)/protocol.o $(LOGGER_LIB)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)

$(TEST): $(TESTS)/test_server.cpp $(BUILD)/protocol.o $(BUILD)/session_registry.o $(LOGGER_LIB)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)


$(BUILD)/%.o: $(SRC)/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

test: $(TEST)
	./$(TEST)