# ES-DE UWP (Xbox Dev Mode) com RomM

Port próprio do [ES-DE](https://gitlab.com/es-de/emulationstation-de) 3.4.1 para UWP, feito para rodar no Xbox em Modo Desenvolvedor. Ele segue a mesma lógica dos ports UWP do Dan Price ([danprice142](https://github.com/danprice142)):

- App C++/WinRT com renderizador Direct3D 12.
- Dependências via vcpkg com triplet UWP.
- Emuladores abertos por ativação de protocolo, por exemplo `retroarch:?cmd=...&launchOnExit=esde:`.

Já inclui a **integração com o RomM**: importar a biblioteca e baixar o jogo direto para o disco das ROMs no primeiro lançamento. Veja [ROMM.md](../ROMM.md).

> **Status:** este código foi escrito e verificado num ambiente Linux, sem Visual Studio. **Ainda não foi compilado com MSVC nem testado num Xbox.** Leia "O que foi verificado" e "Riscos conhecidos" antes de começar.

---

## O que muda em relação ao ES-DE normal

| Área | ES-DE Windows | Este port UWP |
|---|---|---|
| Renderização | OpenGL 3.3 (GLEW) | **Direct3D 12** (`es-core/src/renderers/RendererD3D12.*`), shaders HLSL compilados em tempo de execução |
| Lançar jogos | `CreateProcessW` | **Ativação de protocolo** (`Windows::System::Launcher::LaunchUriAsync`) |
| Voltar ao ES-DE | o processo do emulador termina | o emulador ativa `esde:` (`launchOnExit`) |
| Dados (`ES-DE/`) | `%USERPROFILE%\ES-DE` | **`E:\ES-DE`** (ou a pasta `LocalState` do app se `E:` não for gravável) |
| ROMs | `%USERPROFILE%\ROMs` | **`E:\ROMs`** (configurável no menu) |
| Manuais PDF | processo `es-pdf-convert.exe` | biblioteca linkada no app (opcional, `ES_UWP_PDF`) |
| Scripts de evento, reiniciar/desligar o sistema, volume do sistema, status de Bluetooth/Wi-Fi/bateria | sim | desativados (não existem para apps UWP) |

Todo o código específico fica atrás de `#if defined(ES_UWP)` e `USE_D3D12_RENDERER`, então os builds de Linux, Windows e macOS continuam iguais.

### Como os emuladores são abertos

O port usa o `es_systems.xml` do Windows sem alterações. As variáveis `%EMULATOR_X%` são resolvidas pelo arquivo `resources/systems/uwp/es_find_rules.xml`, com um tipo de regra novo, `uwpprotocol`:

```xml
<emulator name="RETROARCH">
    <rule type="uwpprotocol">
        <entry>retroarch:?cmd=retroarch %ARGS%&amp;launchOnExit=%RETURNURI%</entry>
    </rule>
</emulator>
<core name="RETROARCH">
    <rule type="corepath">
        <entry>uwp:cores</entry>   <!-- "uwp:" = dentro do pacote do emulador, sem checar existência -->
    </rule>
</core>
```

Um jogo de SNES vira esta URI:

```
retroarch:?cmd=retroarch%20-L%20cores%5Csnes9x_libretro.dll%20%22E%3A%5CROMs%5Csnes%5CJogo%20%26%20Cia.sfc%22&launchOnExit=esde%3A
```

O RetroArch decodifica a URI de volta para `retroarch -L cores\snes9x_libretro.dll "E:\ROMs\snes\Jogo & Cia.sfc"`. Esse formato foi conferido no `uwp/uwp_main.cpp` do RetroArch.

Para mudar ou adicionar emuladores, crie `E:\ES-DE\custom_systems\es_find_rules.xml` com as suas regras. Esse arquivo tem prioridade sobre o padrão. Um comando em `es_systems.xml` também pode ser uma URI direta, por exemplo `xboxgamelist:%BASENAME%`.

---

## Requisitos para compilar

- Windows 10 ou 11.
- **Visual Studio 2022**, com:
  - a carga de trabalho "Desenvolvimento para a Plataforma Universal do Windows";
  - "Ferramentas de build do C++ v143";
  - o Windows SDK 10.0.22621 ou mais novo.
- **CMake 3.21+** (já vem com o VS).
- **vcpkg**:

  ```powershell
  git clone https://github.com/microsoft/vcpkg C:\vcpkg
  C:\vcpkg\bootstrap-vcpkg.bat
  setx VCPKG_ROOT C:\vcpkg
  ```

- Um Xbox em Modo Desenvolvedor, com o RetroArch UWP e os outros emuladores que você for usar instalados.

## Compilar

No **Developer PowerShell for VS 2022**, na raiz do repositório:

```powershell
cmake -S uwp -B build-uwp -G "Visual Studio 17 2022" -A x64 `
      "-DCMAKE_SYSTEM_NAME=WindowsStore" "-DCMAKE_SYSTEM_VERSION=10.0" `
      -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
      -DVCPKG_TARGET_TRIPLET=x64-uwp
```

Na primeira configuração o vcpkg compila todas as dependências listadas em `uwp/vcpkg.json` (curl, FFmpeg, FreeImage, FreeType, HarfBuzz, ICU, libgit2, gettext, pugixml, SDL2, zlib). Isso pode levar mais de uma hora.

Depois:

```powershell
cmake --build build-uwp --config Release
```

Ou abra `build-uwp\ES-DE-UWP.sln` no Visual Studio, com a configuração **Release / x64**.

### Manuais em PDF (opcional)

O Poppler usa licença GPL e, no UWP, fica linkado dentro do app. Por isso o suporte vem desligado. Para ativar, acrescente estas duas opções ao comando `cmake` de configuração acima:

```powershell
-DES_UWP_PDF=ON -DVCPKG_MANIFEST_FEATURES=pdf
```

## Gerar o pacote e instalar no Xbox

1. **Certificado de teste** (uma vez só). O publisher precisa ser igual a `UWP_PUBLISHER`, que por padrão é `CN=ES-DE UWP`:

   ```powershell
   $cert = New-SelfSignedCertificate -Type Custom -Subject "CN=ES-DE UWP" -KeyUsage DigitalSignature `
           -FriendlyName "ES-DE UWP" -CertStoreLocation "Cert:\CurrentUser\My" `
           -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
   $pwd = ConvertTo-SecureString -String "esde" -Force -AsPlainText
   Export-PfxCertificate -Cert $cert -FilePath C:\esde-uwp.pfx -Password $pwd
   ```

   Depois reconfigure o projeto acrescentando `-DUWP_CERTIFICATE=C:/esde-uwp.pfx` ao comando `cmake` de configuração.

2. **Pacote:** no Visual Studio, clique com o botão direito no projeto `ES-DE` e escolha *Publicar* > *Criar Pacotes de Aplicativos* > *Sideloading* > x64 / Release. O resultado é um `.msix` com as DLLs, os temas e os recursos dentro.
3. **Instalar:** abra o Device Portal do Xbox (`https://IP-DO-XBOX:11443`), vá em *Add* e envie o `.msix` e o certificado `.cer`.
   - Outra opção é publicar direto pelo Visual Studio, em *Depurar* > *Máquina Remota* > IP do Xbox.
4. **No Xbox:** no Dev Home, mude o tipo do app de "App" para **"Game"**, para ter mais memória e GPU.

## Primeiro uso

- Na primeira execução são criados `E:\ES-DE` (configurações, gamelists, mídia) e a estrutura de pastas das ROMs.
- Coloque as ROMs em `E:\ROMs\<sistema>`, por exemplo `E:\ROMs\snes`. Também dá para usar *Utilitários > Criar/Atualizar diretórios de sistemas*.
- Se o emulador padrão de um sistema não for o RetroArch e não tiver regra `uwpprotocol`, escolha outro em *Outras configurações > Emuladores alternativos*.
- **RomM:** vá em *Menu > Utilitários > ROMM*:
  1. Preencha a URL do servidor e o token da API.
  2. Use *Testar conexão* e depois *Importar jogos do RomM*.
  3. Ao abrir um jogo importado, ele é baixado para `E:\ROMs\...` e só então o emulador é aberto.

---

## O que foi verificado (sem Visual Studio)

- **Shaders HLSL:** os 7 shaders (core, blur horizontal e vertical, scanlines) compilam com o compilador HLSL do `glslang`.
- **Renderizador D3D12:** compila com os headers D3D12/DXGI do MinGW, `-Wall` limpo, tanto no caminho CoreWindow (UWP) quanto no HWND (Win32).
- **Todos os fontes do ES-DE para Windows:** passaram por uma varredura de sintaxe com MinGW, nas configurações `ES_UWP` + `WINAPI_FAMILY=WINAPI_FAMILY_APP`. A varredura achou e corrigiu usos de APIs Win32 de desktop proibidas no UWP:
  - `CreateFileW`, `SetThreadLocale`, `GetAdaptersInfo`, `SetProcessDPIAware`;
  - APIs de console;
  - `CreateProcessW` (no lançamento de jogos e no conversor de PDF);
  - funções Win32 usadas pela biblioteca CImg.

  Restam só 2 erros, os mesmos que o ES-DE original dá nessa checagem, porque o MinGW não tem uma extensão da STL do MSVC.
- **Build Windows desktop normal:** a mesma varredura sem `ES_UWP` dá exatamente os mesmos erros do ES-DE original, então nada mudou para ele.
- **Build Linux:** compila e roda. A importação e o download do RomM foram testados num ES-DE de verdade (em tela virtual) contra um servidor RomM simulado.
- **Montagem da URI de lançamento:** testada e decodificada de volta com um parser de query padrão. Inclui nomes de arquivo com espaços, parênteses e `&`.

## Riscos conhecidos e o que conferir primeiro

1. **SDL2 para UWP no vcpkg.** Se o port `sdl2` do vcpkg não compilar para `x64-uwp`, compile o SDL2 pelo projeto oficial `VisualC-WinRT\SDL-UWP.vcxproj` (ou use o fork do SDL do Dan). Depois aponte `SDL2_DIR` para ele.
2. **Outras dependências no vcpkg UWP.** FFmpeg, FreeImage, libgit2, ICU e gettext podem precisar de ajustes no vcpkg para UWP. O port do Dan usa essas mesmas bibliotecas como DLLs (`avcodec-62.dll`, `FreeImage.dll`, `git2.dll`, `intl-8.dll`…), o que indica que é possível.
3. **Protocolos dos emuladores standalone.**
   - O formato do RetroArch foi verificado no código.
   - Dolphin, XBSX2, PPSSPP, DuckStation, Flycast, Cemu, xemu, Xenia, Azahar e Citra seguem a mesma convenção `?cmd=` dos ports do Dan, mas não foram verificados no código.
   - RPCS3 e shadPS4 têm nomes de protocolo presumidos.
   - Ajuste no `custom_systems/es_find_rules.xml` se algum não abrir.
4. **Renderizador D3D12.** Ele foi escrito para reproduzir exatamente o OpenGL, incluindo cantos arredondados, recorte, blur do fundo do menu, scanlines e a leitura do fundo para cache. Mas nunca rodou numa GPU de verdade. Se algo aparecer de cabeça para baixo ou com cores trocadas, olhe primeiro `shaderPostprocessing()` em `RendererD3D12.cpp`.
5. **Rotação de tela de 90°/270°** não é suportada no pós-processamento D3D12. Não faz falta no Xbox.

## Arquivos do port

| Arquivo | Função |
|---|---|
| `es-core/src/renderers/RendererD3D12.h/.cpp` | Renderizador Direct3D 12 |
| `es-core/src/renderers/ShadersD3D12.h` | Shaders HLSL, tradução dos GLSL em `resources/shaders/glsl` |
| `es-core/src/utils/PlatformUtilUWP.h/.cpp` | Lançamento por URI, pastas, detecção do Xbox (C++/WinRT) |
| `resources/systems/uwp/es_find_rules.xml` | Regras `uwpprotocol` dos emuladores |
| `uwp/CMakeLists.txt`, `uwp/vcpkg.json` | Build UWP |
| `uwp/Package.appxmanifest.in`, `uwp/Assets/` | Manifesto (protocolo `esde:`, capacidades) e ícones |
| `uwp/src/UWPMain.cpp` | Ponto de entrada (`SDL_WinRTRunApp`) |
| `uwp/src/ConvertPDFStub.cpp` | Usado quando o suporte a PDF está desligado |
| `es-app/src/RomM.*`, `es-app/src/guis/GuiRomM*` | Integração com o RomM |
