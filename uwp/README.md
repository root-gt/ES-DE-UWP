# ES-DE UWP (Xbox Dev Mode) com RomM

Port próprio do [ES-DE](https://gitlab.com/es-de/emulationstation-de) 3.4.1 para UWP, feito para rodar no Xbox em Modo Desenvolvedor. Ele segue a mesma lógica dos ports UWP do Dan Price ([danprice142](https://github.com/danprice142)):

- App C++/WinRT com renderizador Direct3D 12.
- Dependências via vcpkg com triplet UWP.
- Emuladores abertos por ativação de protocolo, por exemplo `retroarch:?cmd=...&launchOnExit=esde:`.

Já inclui a **integração com o RomM**: importar a biblioteca e baixar o jogo direto para o disco das ROMs no primeiro lançamento. Veja [ROMM.md](../ROMM.md).

> **Status:** o pacote `.msix` é compilado pelo GitHub Actions (Visual Studio 2022 + vcpkg) e, a cada build, instalado e aberto num Windows de verdade. Lá o ES-DE inicia com o Direct3D 12 e faz a importação e o download de um jogo do RomM dentro do sandbox do UWP. **Ainda não foi testado num Xbox.** Leia "O que é testado" e "Riscos conhecidos".

## Baixar o pacote pronto

Cada push no `main` gera o pacote na aba **Actions** do repositório, workflow **UWP**. Abra a execução mais recente com ✅ e baixe o artifact **ES-DE-UWP-x64**. Ele contém:

| Arquivo | Para quê |
|---|---|
| `ES-DE_3.4.1.0_x64.msix` | o app |
| `ES-DE_UWP.cer` | certificado que assina o pacote |
| `Dependencies/Microsoft.VCLibs.x64.14.00.appx` | runtime do C++, instale junto se o Device Portal pedir |

Tags `v*` (por exemplo `v3.4.1-uwp1`) também publicam esses arquivos numa Release.

**Certificado:** sem configuração, cada build gera um certificado novo. Para atualizar o app no Xbox sem desinstalar, use sempre o mesmo certificado: crie um `.pfx` com o assunto `CN=ES-DE UWP` (comando na seção "Gerar o pacote") e cadastre dois *secrets* no repositório:
- `UWP_CERT_PFX_BASE64`: o `.pfx` em base64, gerado com `[Convert]::ToBase64String([IO.File]::ReadAllBytes("C:\esde-uwp.pfx"))`;
- `UWP_CERT_PASSWORD`: a senha do `.pfx`.

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
| Baixador de temas | sim (libgit2) | **não disponível**: a libgit2 depende de APIs Win32 de desktop. Os temas Linear, Modern e Slate vêm no pacote, e outros podem ser copiados para `E:\ES-DE\themes` |
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
- **vcpkg na versão `2026.07.29`**, a última que ainda tem o FreeImage:

  ```powershell
  git clone --branch 2026.07.29 https://github.com/microsoft/vcpkg C:\vcpkg
  C:\vcpkg\bootstrap-vcpkg.bat
  setx VCPKG_ROOT C:\vcpkg
  ```

- **Python 3** ou o `msgfmt` do gettext, para compilar as traduções (`uwp/tools/msgfmt.py` é usado se o `msgfmt` não existir).

- Um Xbox em Modo Desenvolvedor, com o RetroArch UWP e os outros emuladores que você for usar instalados.

## Compilar

No **Developer PowerShell for VS 2022**, na raiz do repositório:

```powershell
cmake -S uwp -B build-uwp -G "Visual Studio 17 2022" -A x64 `
      "-DCMAKE_SYSTEM_NAME=WindowsStore" "-DCMAKE_SYSTEM_VERSION=10.0" `
      "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
      "-DVCPKG_TARGET_TRIPLET=x64-uwp"
```

As aspas são necessárias no PowerShell, que quebra argumentos como `-DCMAKE_SYSTEM_VERSION=10.0` em dois.

Na primeira configuração o vcpkg compila as dependências de `uwp/vcpkg.json`: curl (com OpenSSL), FFmpeg, FreeImage, FreeType, ICU, gettext, pugixml, SDL2 e zlib. Isso leva cerca de uma hora. Ajustes para UWP:
- **FreeImage:** usa o port de `uwp/vcpkg-ports/freeimage` (registrado em `uwp/vcpkg-configuration.json`), sem os plugins EXR e RAW, sem o FreeImagePlus e sem carregar plugins externos (DLLs). OpenEXR e LibRaw não compilam para UWP.
- **HarfBuzz:** é compilado pelo próprio projeto a partir do arquivo único `harfbuzz.cc`, porque o build do vcpkg (meson) falha no UWP.

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

2. **Pacote:** no Visual Studio, clique com o botão direito no projeto `ES-DE` e escolha *Publicar* > *Criar Pacotes de Aplicativos* > *Sideloading* > x64 / Release. O resultado é um `.msix` com as DLLs, os temas e os recursos dentro. Pela linha de comando, como no CI:

   ```powershell
   cmake --build build-uwp --config Release -- /p:AppxBundle=Never /p:GenerateAppxPackageOnBuild=true `
         /p:UapAppxPackageBuildMode=SideloadOnly "/p:AppxPackageDir=$PWD\package\\"
   ```
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

## O que é testado

O workflow `.github/workflows/uwp.yml` roda a cada push:

1. **Testes no Linux** (`tests/`):
   - 62 verificações do RomM contra um servidor RomM simulado (`tests/romm/mock_romm.py`): conexão e token, mapeamento de plataformas, importação com paginação, metadados no `gamelist.xml`, mídia, reimportação sem duplicar, download, extração de ZIP, IDs desatualizados e cancelamento.
   - 61 verificações das URIs de lançamento, cobrindo as 16 regras `uwpprotocol`. Cada URI é decodificada de volta como o RetroArch faz, e precisa dar o comando original.
2. **Build do MSIX** com Visual Studio 2022 e vcpkg (`x64-uwp`).
3. **Smoke test** (`uwp/ci/smoke-test.ps1`) num Windows Server 2022, sem GPU:
   - instala o `.msix` assinado e confere se as traduções e os temas estão no pacote;
   - abre o ES-DE e confere no log que o **Direct3D 12** foi inicializado (pelo WARP, o renderizador por software do Windows) e que a inicialização terminou sem o app fechar;
   - abre de novo com um servidor RomM configurado e a opção oculta `RomMSelfTestGame`. **Dentro do sandbox do UWP**, o ES-DE importa a biblioteca (265 jogos e 540 arquivos de mídia) e baixa e extrai um jogo de um ZIP;
   - tira capturas de tela, confere que há conteúdo desenhado e guarda os logs e as capturas no artifact `smoke-test`.

O workflow **UWP smoke test** também pode ser disparado à mão na aba Actions, informando o ID de uma execução do UWP, para testar de novo um pacote já compilado.

## Riscos conhecidos e o que conferir primeiro

1. **Xbox.** O pacote foi testado no Windows, não no Xbox. No console, confira principalmente o acesso a `E:\` (a capacidade `broadFileSystemAccess` precisa ser permitida nas configurações de privacidade do app) e a volta do emulador para o ES-DE (`launchOnExit=esde:`).
2. **Protocolos dos emuladores standalone.**
   - O formato do RetroArch foi verificado no código.
   - Dolphin, XBSX2, PPSSPP, DuckStation, Flycast, Cemu, xemu, Xenia, Azahar e Citra seguem a mesma convenção `?cmd=` dos ports do Dan, mas não foram verificados no código.
   - RPCS3 e shadPS4 têm nomes de protocolo presumidos.
   - Ajuste no `custom_systems/es_find_rules.xml` se algum não abrir.
3. **Renderizador D3D12 numa GPU de verdade.** No CI ele roda no WARP. Se algo aparecer de cabeça para baixo ou com cores trocadas no Xbox, olhe primeiro `shaderPostprocessing()` em `RendererD3D12.cpp`.
4. **Rotação de tela de 90°/270°** não é suportada no pós-processamento D3D12. Não faz falta no Xbox.

## Arquivos do port

| Arquivo | Função |
|---|---|
| `es-core/src/renderers/RendererD3D12.h/.cpp` | Renderizador Direct3D 12 |
| `es-core/src/renderers/ShadersD3D12.h` | Shaders HLSL, tradução dos GLSL em `resources/shaders/glsl` |
| `es-core/src/utils/PlatformUtilUWP.h/.cpp` | Lançamento por URI, pastas, detecção do Xbox (C++/WinRT) |
| `resources/systems/uwp/es_find_rules.xml` | Regras `uwpprotocol` dos emuladores |
| `uwp/CMakeLists.txt`, `uwp/vcpkg.json` | Build UWP |
| `uwp/vcpkg-ports/freeimage/` | Port do FreeImage ajustado para UWP |
| `uwp/tools/msgfmt.py` | Compila as traduções sem o gettext instalado |
| `uwp/ci/smoke-test.ps1`, `.github/workflows/` | CI: build, testes e smoke test |
| `tests/` | Testes do RomM e das URIs de lançamento |
| `uwp/Package.appxmanifest.in`, `uwp/Assets/` | Manifesto (protocolo `esde:`, capacidades) e ícones |
| `uwp/src/UWPMain.cpp` | Ponto de entrada (`SDL_WinRTRunApp`) |
| `uwp/src/ConvertPDFStub.cpp` | Usado quando o suporte a PDF está desligado |
| `es-app/src/RomM.*`, `es-app/src/guis/GuiRomM*` | Integração com o RomM |
