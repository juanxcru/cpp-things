include makemacs

#all: $(MAIN) $(TEST)

#main: $(MAIN)

lib: $(TARGET)
logger: $(TARGET)

# MAIN
$(MAIN): $(MAINO) $(LIBO)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)


# TEST
$(TEST): $(TESTO) $(LIBO)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

#LIB
$(TARGET): $(LIBO) 
	rm -f $(TARGET) 
	$(LIBAR) $(TARGET) $(LIBO)


%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f src/*.o tests/*.o $(MAIN) $(TEST)
	rm -f $(TARGET)

test: $(TEST)
	./$(TEST) 2>&1

# run: