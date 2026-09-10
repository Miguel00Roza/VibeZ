# VibeZ
Software desenvolvido para suprir a necessidade do compartilhamento de tela usando a menor quantidade de recursos possivel do pc

---

## Requisitos

- Windows 10/11 64-bits
- CMake
- MSVC
- vcpkg
- Git

### 1. Clone do projeto

````bash
git clone https://github.com/Miguel00Roza/VibeZ.git
cd VibeZ
````

### 2. Instale as dependências

````
vcpkg install
````

### 3. Configure o CMake

````
cmake --preset windows-debug
````

Caso necessario alterar o caminho de ``/.../vcpkg.cmake`` em ``CMakePresets.json``

### 4. Compile

````
cmake --build build
````