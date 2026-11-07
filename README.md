# VibeZ

VibeZ é uma aplicação nativa para comunicação em tempo real. A fundação atual entrega um servidor headless com canais, presença e relay UDP validado, um cliente com interface gráfica Qt 6 e um cliente de console, ambos transmitindo voz ponta a ponta usando WASAPI e Opus. Vídeo e autenticação ainda estão em desenvolvimento.

## Estado atual

- `VibeZServer.exe`: servidor TCP/UDP, com canal `Lobby`, criação/remoção de canais, entrada/saída, mute e estado de compartilhamento.
- `VibeZ.exe`: cliente com interface gráfica Qt 6 (Widgets). Permite conectar, criar/entrar em canais, ver participantes e níveis de voz, mutar o microfone, ajustar o volume e silenciar cada participante, anunciar compartilhamento de tela e pedir keyframe.
- `VibeZConsole.exe`: cliente de linha de comando mantido para diagnóstico e automação.
- Protocolo binário **versão 2**: TCP para controle e UDP para mídia. O servidor só encaminha UDP de usuários autenticados no mesmo canal, descarta áudio de usuários mutados e vídeo de usuários que não estão compartilhando. A versão 2 não é compatível com clientes/servidores versão 1.
- Áudio ponta a ponta: captura WASAPI em modo compartilhado (48 kHz mono PCM16), quadros Opus de 20 ms, jitter buffer por usuário, decode, mixagem por usuário e reprodução WASAPI.
- Cliente sem bloqueio: `ClientSession` compartilha rede e áudio entre a GUI e o console; a interface apenas consulta eventos e níveis por um timer de 20 ms.
- Reconexão automática do cliente, com estado exposto por eventos.
- Testes de protocolo, áudio, dispositivos WASAPI reais, integração do servidor, rede e voz sobre o transporte via CTest.

Ainda não há autenticação por senha (adiada explicitamente nesta fase), TLS/DTLS, criptografia, codec de vídeo conectado ao transporte nem transmissão real de tela. O botão **Compartilhar tela** apenas anuncia o estado e o tamanho do monitor principal; nenhum frame é capturado ou enviado. O relay de mídia é via servidor; não há conexão P2P nem fallback STUN/TURN. Para uso fora da LAN, hospede o servidor em uma máquina acessível publicamente e libere **a mesma porta TCP e UDP**; CGNAT pode impedir conexões de entrada.

## Compilação (Windows 10/11 x64)

Requer Visual Studio 2026 com ferramentas C++ e CMake 3.25 ou superior. As dependências são declaradas em `vcpkg.json` (Opus, spdlog, stb e Qt 6 `qtbase` com Widgets e `windeployqt`). Abra um *Developer Command Prompt for VS x64* e instale-as uma vez:

```powershell
vcpkg install --triplet x64-windows
```

Em seguida:

```powershell
cmake --preset windows-debug
cmake --build out/build/vibez-windows-debug --config Debug
ctest --test-dir out/build/vibez-windows-debug -C Debug --output-on-failure
```

Os executáveis serão criados em `out/build/vibez-windows-debug/Debug/`. O `opus.dll` é copiado automaticamente e o `windeployqt` copia as DLLs e plugins do Qt para o lado de `VibeZ.exe`. Se o Qt estiver em outro prefixo, ajuste `CMAKE_PREFIX_PATH` ou a variável de ambiente `PATH`.

## Execução local

Inicie o servidor em um terminal:

```powershell
.\VibeZServer.exe 28765
```

Abra dois clientes gráficos (um por participante) e informe servidor, porta e apelido:

```powershell
.\VibeZ.exe
```

Para automatizar a conexão:

```powershell
.\VibeZ.exe --host 127.0.0.1 --port 28765 --nickname Alice
```

Na janela, crie um canal (por exemplo, `Amigos`) e clique em **Entrar no canal** nos dois clientes. O áudio flui automaticamente; use **Microfone ativo/mudo**, os botões **Silenciar** e os controles de volume de cada participante.

O cliente de console continua disponível:

```powershell
.\VibeZConsole.exe 127.0.0.1 28765 Alice
```

Comandos do cliente de console:

- `/create <nome>`: cria um canal.
- `/join <id>`: entra em um canal.
- `/leave`: volta para o `Lobby`.
- `/mute <0|1>`: muta/desmuta o microfone local (o servidor também descarta áudio de usuários mutados).
- `/share <0|1> <largura> <altura>`: anuncia/remove o estado de compartilhamento de tela.
- `/keyframe <id>`: solicita um keyframe ao participante que está compartilhando.
- `/level`: mostra nível local, pacotes de voz recebidos e nível por usuário.
- `/quit`: encerra o cliente.

## Testes

`ctest` cobre:

- `vibez_protocol_tests`: framing, validação e limites do protocolo v2.
- `vibez_audio_tests`: Opus, resampler, framer, jitter buffer, filas e mixer.
- `vibez_audio_device_tests`: captura e reprodução WASAPI reais (requer dispositivos ativos).
- `vibez_server_integration_tests`: canais, presença e relay UDP.
- `vibez_network_tests`: reconexão, presença, erros, keyframe e relay pelo cliente de rede.
- `vibez_voice_network_tests`: encode Opus → UDP → decode → mixagem e isolamento por canal.

O teste manual com dois clientes na mesma máquina confirmou conexão, presença, entrada no mesmo canal e tráfego de voz bidirecional (contador de pacotes recebidos e níveis por usuário não nulos). Um teste de fumaça com duas instâncias da GUI Qt (`QT_QPA_PLATFORM=offscreen --auto-connect --quit-after`) confirma conexão e presença dos dois participantes; a validação visual da janela e o teste auditivo ainda dependem de execução interativa.

## Limitações conhecidas e próximas fases

- O compartilhamento de tela é apenas anunciado na GUI; captura, codificação e transmissão de vídeo ainda não estão integradas.
- Seleção de dispositivos de áudio ainda usa os dispositivos padrão do sistema.
- O fallback de formato do WASAPI (resampler manual) existe, mas a negociação de formato alternativa ainda não está implementada.
- Captura/renderização de tela existentes em `client/capture` e `client/render` ainda não foram integradas ao transporte; há um problema conhecido de dispositivos D3D11 separados e o renderizador está incompleto.
- Autenticação, TLS/DTLS e endurecimento completo do servidor (limites, filas e rate limiting) permanecem pendentes.
