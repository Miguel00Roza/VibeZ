# VibeZ

VibeZ é uma aplicação nativa para comunicação em tempo real. A fundação atual entrega um servidor headless com canais, presença e relay UDP validado, um cliente com interface gráfica Qt 6 e um cliente de console, ambos transmitindo voz ponta a ponta usando WASAPI e Opus, com compartilhamento de tela e do som do computador. Autenticação ainda está em desenvolvimento.

## Estado atual

- `VibeZServer.exe`: servidor TCP/UDP, com canal `Lobby`, criação/remoção de canais, entrada/saída, mute e estado de compartilhamento.
- `VibeZ.exe`: cliente com interface gráfica Qt 6 (Widgets). Permite conectar, criar/entrar em canais, ver participantes e níveis de voz, mutar o microfone, ajustar o volume de voz e o volume do som da tela de cada participante, compartilhar a tela com o som do computador, pedir keyframe e reiniciar o áudio sem desconectar.
- `VibeZConsole.exe`: cliente de linha de comando mantido para diagnóstico e automação.
- Protocolo binário **versão 4**: TCP para controle e UDP para mídia. O áudio da voz e o som do computador viajam como fluxos Opus independentes, identificados por flag. O servidor só encaminha UDP de usuários autenticados no mesmo canal, descarta áudio de usuários mutados e vídeo/som de tela de quem não está compartilhando. Keepalives UDP mantêm o mapeamento NAT e ensinam o endpoint real de cada cliente. A versão 4 não é compatível com clientes/servidores anteriores.
- Áudio ponta a ponta: captura WASAPI em modo compartilhado (48 kHz mono PCM16), quadros Opus de 20 ms (24 kbps para voz e 64 kbps para som da tela), jitter buffer independente por fluxo e usuário, decode, mixagem por usuário e reprodução WASAPI. As threads de áudio usam MMCSS e opt-out de throttling para continuar estáveis com a janela minimizada, e as streams WASAPI se recriam automaticamente após erros de dispositivo.
- Compartilhamento de tela: captura do monitor principal em JPEG, fragmentação em datagramas UDP, remontagem por `(sessão, frame)` com múltiplos frames em voo, envio ritmado (~4 Mbps) e orçamento de bytes por frame para evitar filas e travamentos. O visualizador remoto é atualizado na GUI.
- Visualizador no navegador: o próprio servidor reutiliza os fragmentos de vídeo já retransmitidos e publica a tela compartilhada em uma página web, sem instalar nada no PC ou no celular. A página tem seletor de quem está compartilhando, acompanhamento automático do compartilhamento mais recente, status ao vivo, FPS, tela cheia, download do quadro atual e reconexão automática.
- Cliente sem bloqueio: `ClientSession` compartilha rede, áudio e vídeo entre a GUI e o console; a interface apenas consulta eventos e níveis por um timer de 20 ms.
- Reconexão automática do cliente, com estado exposto por eventos.
- Testes de protocolo, áudio, dispositivos WASAPI reais, integração do servidor, rede e voz sobre o transporte via CTest.

Ainda não há autenticação por senha (adiada explicitamente nesta fase), TLS/DTLS nem criptografia. O vídeo usa JPEG sobre UDP (sem codec diferencial) e o relay de mídia é via servidor; não há conexão P2P nem fallback STUN/TURN. Para uso fora da LAN, hospede o servidor em uma máquina acessível publicamente e libere **a mesma porta TCP e UDP**; CGNAT pode impedir conexões de entrada.

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

Os executáveis serão criados em `out/build/vibez-windows-debug/Debug/`. O `opus.dll` e as DLLs/plugins do Qt são copiados automaticamente para o lado de `VibeZ.exe`. Se o Qt estiver em outro prefixo, ajuste `CMAKE_PREFIX_PATH` ou a variável de ambiente `PATH`.

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

Na janela, crie um canal (por exemplo, `Amigos`) e clique em **Entrar no canal** nos dois clientes. O áudio flui automaticamente; use **Microfone ativo/mudo**, os botões **Silenciar**, o controle **Volume** (voz) e o controle **Tela** (som do computador compartilhado) de cada participante. **Compartilhar tela** abre as opções de FPS, qualidade, resolução e som do computador; **Pedir keyframe** força um quadro completo ao participante selecionado e **Reiniciar áudio** recria as streams WASAPI sem derrubar a sessão.

## Visualizador no navegador

Ao iniciar, o servidor sobe também uma página que mostra a tela compartilhada para qualquer navegador na rede. Ele imprime os endereços prontos:

```text
Web viewer for the shared screen:
  http://192.168.15.11:8080
Open it on a PC or phone; allow TCP 8080 through the firewall.
```

Abra o endereço no PC ou no celular (o telefone precisa estar na mesma rede ou ter acesso à porta). A página funciona sem instalar aplicativo e se adapta a telas pequenas. Enquanto ninguém compartilha, ela mostra o aviso de espera; quando alguém compartilha, a tela aparece automaticamente. Com mais de uma pessoa compartilhando, use o seletor no topo: **Automático (mais recente)** segue quem começou a compartilhar por último, e escolher um nome fixa aquele participante.

Opções do servidor:

```powershell
.\VibeZServer.exe 28765                       # voz na 28765, página na 8080
.\VibeZServer.exe 28765 --web-port 9000       # muda a porta da página
.\VibeZServer.exe 28765 --web-bind 127.0.0.1  # só aceita conexões da própria máquina
.\VibeZServer.exe 28765 --no-web              # desliga a página
```

A página usa o mesmo relay de vídeo do servidor: não há configuração adicional no cliente e o compartilhamento continua funcionando na GUI normalmente. Para acesso de outra rede, libere a porta TCP da página (padrão 8080) além da porta TCP/UDP da voz. A página não tem senha e mostra a tela para quem alcançar a porta; use `--web-bind` ou uma rede confiável quando isso importar.

Rotas expostas:

- `/`: página do visualizador.
- `/api/streams`: JSON com quem está compartilhando, nome, canal, resolução e FPS.
- `/stream.mjpeg`: transmissão contínua (`multipart/x-mixed-replace`).
- `/snapshot.jpg`: quadro atual em JPEG, usado como alternativa quando o navegador recusa MJPEG e também para baixar a imagem.

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

- `vibez_protocol_tests`: framing, validação, limites, keepalive e flags de áudio/vídeo do protocolo v4.
- `vibez_audio_tests`: Opus, resampler, framer, jitter buffer, filas e mixer (canais de voz e tela independentes).
- `vibez_audio_device_tests`: captura e reprodução WASAPI reais (requer dispositivos ativos).
- `vibez_server_integration_tests`: canais, presença e relay UDP.
- `vibez_network_tests`: reconexão, presença, erros, keyframe, relay de voz, som de tela e fragmentos de vídeo.
- `vibez_voice_network_tests`: encode Opus → UDP → decode → mixagem, fluxos de voz/tela e isolamento por canal.
- `vibez_web_tests`: remontagem de frames no servidor, seleção/auto-follow de quem compartilha, rotas HTTP, streaming MJPEG ao vivo, limite de espectadores e caminho completo cliente → servidor → página.

O teste manual com dois clientes na mesma máquina confirmou conexão, presença, entrada no mesmo canal e tráfego de voz bidirecional (contador de pacotes recebidos e níveis por usuário não nulos). Um teste de fumaça com duas instâncias da GUI Qt (`QT_QPA_PLATFORM=offscreen --auto-connect --quit-after`) confirma conexão e presença dos dois participantes; a validação visual da janela e o teste auditivo ainda dependem de execução interativa.

## Limitações conhecidas e próximas fases

- O vídeo é enviado como frames JPEG independentes; não há codec diferencial, então o consumo de banda é maior que o de codecs como H.264/AV1. A página do navegador também recebe JPEG puro (cerca de 6 Mbps por espectador em 10 fps), sem áudio, sem autenticação e sem criptografia.
- Seleção de dispositivos de áudio ainda usa os dispositivos padrão do sistema.
- O fallback de formato do WASAPI (resampler manual) existe, mas a negociação de formato alternativa ainda não está implementada.
- O renderizador D3D11 existente em `client/render` não está integrado; a pré-visualização remota usa QLabel/QPixmap.
- Autenticação, TLS/DTLS e endurecimento completo do servidor (limites, filas e rate limiting) permanecem pendentes.
