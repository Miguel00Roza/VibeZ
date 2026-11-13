# VibeZ

VibeZ é uma aplicação nativa para comunicação em tempo real. A fundação atual entrega um servidor headless com canais, presença e relay UDP validado, um cliente com interface gráfica Qt 6 e um cliente de console, ambos transmitindo voz ponta a ponta usando WASAPI e Opus, com compartilhamento de tela e do som do computador. Autenticação ainda está em desenvolvimento.

## Estado atual

- `VibeZServer.exe`: servidor TCP/UDP, com canal `Lobby`, criação/remoção de canais, entrada/saída, mute e estado de compartilhamento.
- `VibeZ.exe`: cliente com interface gráfica Qt 6 (Widgets). Permite conectar, criar/entrar em canais, ver participantes e níveis de voz, mutar o microfone, ajustar o volume de voz e o volume do som da tela de cada participante, compartilhar a tela com o som do computador, pedir keyframe e reiniciar o áudio sem desconectar.
- `VibeZConsole.exe`: cliente de linha de comando mantido para diagnóstico e automação.
- Protocolo binário **versão 4**: TCP para controle e UDP para mídia. O áudio da voz e o som do computador viajam como fluxos Opus independentes, identificados por flag. O servidor só encaminha UDP de usuários autenticados no mesmo canal, descarta áudio de usuários mutados e vídeo/som de tela de quem não está compartilhando. Keepalives UDP mantêm o mapeamento NAT e ensinam o endpoint real de cada cliente. A versão 4 não é compatível com clientes/servidores anteriores.
- Áudio ponta a ponta: captura WASAPI em modo compartilhado (48 kHz mono PCM16), quadros Opus de 20 ms (24 kbps para voz e 64 kbps para som da tela), jitter buffer independente por fluxo e usuário, decode, mixagem por usuário e reprodução WASAPI. As threads de áudio usam MMCSS e opt-out de throttling para continuar estáveis com a janela minimizada, e as streams WASAPI se recriam automaticamente após erros de dispositivo.
- Compartilhamento de tela: captura do monitor principal em JPEG, fragmentação em datagramas UDP, remontagem por `(sessão, frame)` com múltiplos frames em voo, envio ritmado com bitrate escolhido (4, 8, 12 ou 20 Mbps; padrão 8 Mbps) e orçamento de bytes por frame derivado de bitrate ÷ FPS para evitar filas e travamentos. O visualizador remoto é atualizado na GUI.
- Visualizador no navegador: o próprio servidor reutiliza os fragmentos de vídeo já retransmitidos e publica a tela compartilhada em uma página web, sem instalar nada no PC ou no celular. A página tem seletor de quem está compartilhando, acompanhamento automático do compartilhamento mais recente, status ao vivo, FPS, tela cheia, download do quadro atual, reconexão automática e **som do computador** com botão de ativar/desativar e volume próprio.
- Cliente sem bloqueio: `ClientSession` compartilha rede, áudio e vídeo entre a GUI e o console; a interface apenas consulta eventos e níveis por um timer de 20 ms.
- Reconexão automática do cliente, com estado exposto por eventos.
- Testes de protocolo, áudio, dispositivos WASAPI reais, integração do servidor, rede, voz e visualizador web (vídeo e som) sobre o transporte via CTest.

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

## Instaladores (Windows)

São dois instaladores independentes, gerados com o Inno Setup 6 (instale uma vez com `winget install JRSoftware.InnoSetup`):

```powershell
powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1 -Product Client
powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1 -Product Server
powershell -ExecutionPolicy Bypass -File installer\build-installer.ps1 -Product All -TestInstall
```

Cada execução compila o alvo em Release, monta um *staging* limpo (`out/deploy/client` ou `out/deploy/server`) — o diretório de build é compartilhado com o Debug, então só DLLs de Release entram —, audita o pacote e chama o Inno Setup. A auditoria recusa DLLs de Debug (detectadas pelas importações `*140d`/`ucrtbased`, não pelo nome), binários do outro produto, `vc_redist` e qualquer dependência não resolvida dentro da pasta (via `dumpbin`). Saída em `out/installer/`:

| Produto | Instalador | Conteúdo |
|---|---|---|
| Cliente | `VibeZ-Setup-<versão>.exe` | `VibeZ.exe`, Qt 6 + plugins, `opus.dll`, runtime C++ app-local |
| Servidor | `VibeZServer-Setup-<versão>.exe` | `VibeZServer.exe`, `opus.dll`, runtime C++ app-local, `vibez-server.ini` |

Os dois são **por usuário, sem UAC**: instalam em `%LOCALAPPDATA%\Programs\VibeZ` e `%LOCALAPPDATA%\Programs\VibeZ Server`, criam atalhos no Menu Iniciar (e opcionalmente na área de trabalho), usam ícone próprio e oferecem português do Brasil e inglês. A versão sai de `-AppVersion` (padrão `0.1.0`); `-TestInstall` faz instalação silenciosa em pasta temporária, checagem dos arquivos, inicialização real do executável e desinstalação.

O instalador do servidor tem uma página extra para escolher **a porta do app** (voz, vídeo e controle — TCP e UDP) e **a porta do site** (visualizador no navegador). Os valores vão para `vibez-server.ini` na pasta de instalação e podem ser passados também em instalações silenciosas:

```powershell
.\VibeZServer-Setup-0.1.0.exe /VERYSILENT /VOICEPORT=29000 /WEBPORT=29001
```

O ícone é gerado por `installer\make-icon.ps1` (variantes `app` e `server`) e usado tanto pelos instaladores quanto pelos recursos embutidos em `VibeZ.exe` e `VibeZServer.exe`.

## Portas do servidor

O servidor lê `vibez-server.ini`, procurado ao lado do executável:

```ini
[server]
voice_port=28765
web_port=8080
web_bind=0.0.0.0
web_enabled=1
```

A precedência é **linha de comando > arquivo > padrões**. Sem o arquivo, valem `28765` (voz/tela) e `8080` (site). Para trocar depois de instalado, edite o arquivo e reinicie — ou rode o instalador de novo, que reaproveita as portas atuais.

```powershell
.\VibeZServer.exe --voice-port 29000 --web-port 29001
.\VibeZServer.exe --config D:\vibez\servidor.ini
.\VibeZServer.exe --print-config      # mostra as portas efetivas e sai
```

O `[porta]` posicional continua valendo para a porta do app, e `--web-port 0` (ou `web_enabled=0`) desliga o site. Portas fora de 1–65535, chaves desconhecidas ou linhas malformadas nunca impedem o servidor de subir: o valor inválido é ignorado com aviso e o padrão é mantido.

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

Na janela, crie um canal (por exemplo, `Amigos`) e clique em **Entrar no canal** nos dois clientes. O áudio flui automaticamente; use **Microfone ativo/mudo**, os botões **Silenciar**, o controle **Volume** (voz) e o controle **Tela** (som do computador compartilhado) de cada participante. **Compartilhar tela** abre as opções de FPS, qualidade, resolução (640 px, 960 px, 1280 px, 1920 px ou a resolução nativa do monitor), bitrate (4, 8, 12 ou 20 Mbps, padrão 8 Mbps) e som do computador; **Pedir keyframe** força um quadro completo ao participante selecionado e **Reiniciar áudio** recria as streams WASAPI sem derrubar a sessão.

## Visualizador no navegador

Ao iniciar, o servidor sobe também uma página que mostra a tela compartilhada para qualquer navegador na rede. Ele imprime os endereços prontos:

```text
Web viewer for the shared screen:
  http://192.168.15.11:8080
Open it on a PC or phone; allow TCP 8080 through the firewall.
```

Abra o endereço no PC ou no celular (o telefone precisa estar na mesma rede ou ter acesso à porta). A página funciona sem instalar aplicativo e se adapta a telas pequenas. Enquanto ninguém compartilha, ela mostra o aviso de espera; quando alguém compartilha, a tela aparece automaticamente. Com mais de uma pessoa compartilhando, use o seletor no topo: **Automático (mais recente)** segue quem começou a compartilhar por último, e escolher um nome fixa aquele participante.

O som do computador de quem compartilha também chega ao navegador. Como os navegadores só liberam áudio depois de um gesto do usuário, clique em **Ativar som** (o botão passa a **Som ligado**); o volume tem um controle próprio, independente da voz e dos outros participantes, e a preferência fica salva no navegador. O servidor decodifica o Opus uma única vez e entrega PCM16 a 48 kHz para todos os ouvintes, então o custo de CPU não cresce com o número de espectadores. O botão volta a **Ativar som** quando o compartilhamento termina e o áudio reconecta sozinho ao trocar de participante ou ao retornar para a aba.

Opções do servidor:

```powershell
.\VibeZServer.exe 28765                       # voz na 28765, página na 8080
.\VibeZServer.exe 28765 --web-port 9000       # muda a porta da página
.\VibeZServer.exe 28765 --web-bind 127.0.0.1  # só aceita conexões da própria máquina
.\VibeZServer.exe 28765 --no-web              # desliga a página
```

A página usa o mesmo relay de vídeo e de som do servidor: não há configuração adicional no cliente e o compartilhamento continua funcionando na GUI normalmente. Para acesso de outra rede, libere a porta TCP da página (padrão 8080) além da porta TCP/UDP da voz. A página não tem senha e mostra a tela para quem alcançar a porta; use `--web-bind` ou uma rede confiável quando isso importar.

Rotas expostas:

- `/`: página do visualizador.
- `/api/streams`: JSON com quem está compartilhando, nome, canal, resolução, FPS, se há som disponível, espectadores e ouvintes.
- `/stream.mjpeg`: transmissão contínua de vídeo (`multipart/x-mixed-replace`).
- `/snapshot.jpg`: quadro atual em JPEG, usado como alternativa quando o navegador recusa MJPEG e também para baixar a imagem.
- `/audio.pcm`: som do computador em PCM16, 48 kHz, mono (`audio/L16`), com `?session=<id>` (use `0` para seguir o compartilhamento mais recente).

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
- `vibez_server_config_tests`: leitura do `vibez-server.ini`, chaves globais/desconhecidas, portas inválidas (0, 70000, texto), `web_port=0`, valores parciais e round-trip de gravação.
- `vibez_network_tests`: reconexão, presença, erros, keyframe, relay de voz, som de tela e fragmentos de vídeo.
- `vibez_voice_network_tests`: encode Opus → UDP → decode → mixagem, fluxos de voz/tela e isolamento por canal.
- `vibez_web_tests`: remontagem de frames no servidor, seleção/auto-follow de quem compartilha, rotas HTTP, streaming MJPEG ao vivo, limite de espectadores, hub de som (decode Opus, PLC em perdas, filtro de voz, início/fim de compartilhamento), rota `/audio.pcm` e caminho completo cliente → servidor → página para vídeo e som.

O teste manual com dois clientes na mesma máquina confirmou conexão, presença, entrada no mesmo canal e tráfego de voz bidirecional (contador de pacotes recebidos e níveis por usuário não nulos). Um teste de fumaça com duas instâncias da GUI Qt (`QT_QPA_PLATFORM=offscreen --auto-connect --quit-after`) confirma conexão e presença dos dois participantes; a validação visual da janela e o teste auditivo ainda dependem de execução interativa.

## Limitações conhecidas e próximas fases

- O vídeo é enviado como frames JPEG independentes; não há codec diferencial, então o consumo de banda é maior que o de codecs como H.264/AV1. No navegador, cada espectador consome o bitrate de vídeo escolhido no cliente (8 Mbps por padrão) mais cerca de 0,77 Mbps de som quando o áudio está ligado, sem autenticação e sem criptografia.
- Seleção de dispositivos de áudio ainda usa os dispositivos padrão do sistema.
- O fallback de formato do WASAPI (resampler manual) existe, mas a negociação de formato alternativa ainda não está implementada.
- O renderizador D3D11 existente em `client/render` não está integrado; a pré-visualização remota usa QLabel/QPixmap.
- Autenticação, TLS/DTLS e endurecimento completo do servidor (limites, filas e rate limiting) permanecem pendentes.
