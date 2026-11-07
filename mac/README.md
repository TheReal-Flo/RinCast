# RinCast Streamer para macOS

Implementação nativa do streamer RinCast para macOS, permitindo transmitir e controlar todas as telas do seu Mac dentro de headsets de realidade virtual (como Meta Quest 3, Quest 3S, Quest Pro, Quest 2 e PICO 4).

## Características

- **ScreenCaptureKit:** Captura de tela com zero-copy direto da GPU da Apple e suporte nativo ao cursor do mouse.
- **VideoToolbox Hardware Encoding:** Codificação em tempo real por hardware em **HEVC (H.265)** e **H.264** com latência mínima (sem B-frames / reordering).
- **CoreGraphics Input Injection:** Controle remoto completo do mouse (movimento, botões esquerdo/direito/meio, scroll suave) e digitação de teclado do headset para o Mac.
- **Protocolo RinCast:** Criptografia ponta a ponta (Monocypher), correção antecipada de erros (FEC XOR), adaptação automática e descoberta sem fios na rede local.

---

## Como Compilar

Requisitos:
- macOS 12.3+ (Apple Silicon ou Intel)
- Xcode Command Line Tools (`xcode-select --install`)
- CMake 3.22+

Para compilar:

```bash
mkdir -p build-mac
cd build-mac
cmake -S ../mac -B .
cmake --build .
```

O binário executável será gerado em:
`build-mac/rincast-streamer-mac`

---

## Como Executar

Execute o binário no terminal:

```bash
./build-mac/rincast-streamer-mac
```

### Permissões Necessárias no macOS

Ao executar pela primeira vez, o macOS solicitará duas permissões de sistema:

1. **Gravação de Tela (Screen Recording):**
   * Caminho: **Ajustes do Sistema > Privacidade e Segurança > Gravação de Tela** (ou *Screen Recording*).
   * Marque a caixa para permitir que o Terminal / RinCast capture o vídeo dos seus monitores.
2. **Acessibilidade (Accessibility):**
   * Caminho: **Ajustes do Sistema > Privacidade e Segurança > Acessibilidade** (ou *Accessibility*).
   * Permita o app para que os controladores do headset possam mover o mouse e digitar no seu Mac.

---

## Conectando pelo Headset

1. Certifique-se de que o Mac e o headset estão conectados à mesma rede Wi-Fi (preferencialmente 5 GHz ou 6 GHz).
2. Abra o aplicativo **RinCast** no seu headset (no Quest, em *Fontes Desconhecidas*).
3. Seu Mac aparecerá automaticamente na lista com o nome do computador.
4. Clique para conectar: todas as suas telas aparecerão no espaço imersivo!
