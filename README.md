# Create-dll-for-Zemax-by-Visual-Studio-2017
Create a Dynamic Link Library (dll) file for Zemax by Microsoft Visual Studio Code.
g++ -c .\diffraction_edge.c -o diffraction_edge.obj
g++ -shared -o diffraction_edge.dll diffraction_edge.obj
