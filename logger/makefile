include makemacs


all: $(MAIN) $(TEST)

main: $(MAIN)

# MAIN
$(MAIN): $(MAINO) $(LIBO)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)


# TEST
$(TEST): $(TESTO) $(LIBO)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)


%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f src/*.o tests/*.o $(MAIN) $(TEST)

test: $(TEST)
	./$(TEST) 2>&1

# run: