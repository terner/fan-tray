CXX := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
PKGS := gtk+-3.0 ayatana-appindicator3-0.1

fan-tray: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -o fan-tray $$(pkg-config --cflags --libs $(PKGS))

clean:
	rm -f fan-tray
