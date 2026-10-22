# Romstore-nx

https://github.com/otakussantos-hash/romstore/blob/main/icon.jpg?raw=true
Catálogo e downloader de ROMs para Nintendo Switch (NRO).

## Menu
Loading (4 s, barra de ponta a ponta + logo) → consoles (carrossel) → jogos → detalhe → download.
A barra do topo (cor do tema) tem a logo; **ZL** (ou "cima" na 1ª linha) faz o alfabeto descer, também na cor do tema.
A letra é reconhecida sozinha pela 1ª letra do título (acentos tratados; números/símbolos vão em `#`).
`sistema` no catálogo deve ser só o nome do console (`PSP`, `DS`...), sem faixas de letras.

## Logo
`romfs/logo.png` é embutida no app (troque e recompile). Para testar sem recompilar, copie um `logo.png` para `sdmc:/switch/romstore/`.
`icon.jpg` (256x256) é o ícone do app no menu de homebrews.

## Pasta de download
Na 1ª vez que baixar algo de um console, o app abre o navegador do cartão SD para escolher a pasta. Vale para todas as letras.
Para mudar: **X** na tela do jogo, ou Y → Configurações → "Pasta de download - <console>".

## Configurações (botão Y)
Tema · imagem de fundo (você escolhe um jpg/png do cartão SD; coloque em `sdmc:/switch/romstore/fundos`) · som ao apertar botões · música de fundo · Snake no download · pasta por console.
Tudo é salvo em `sdmc:/switch/romstore/config.ini`.

## Trocar sons e imagens pelo repositório
No `catalogo.json`:
```json
"imagens": { "DS": "imagens/ds.jpg" },   // imagem do card de cada console
"audio": { "versao": "1", "abertura": "audio/abertura.wav", "clique": "audio/clique.wav", "fundo": "audio/fundo.wav" }
```
Troque o arquivo na pasta `audio/` ou `imagens/` (wav, ogg ou mp3) e aumente o `versao` para os consoles baixarem de novo.
Caminhos relativos são lidos a partir do endereço do catálogo; URLs completas também funcionam.
Alterar `catalogo.json`, `audio/` ou `imagens/` não recompila o app.

## Snake no download
Durante o download: direcional move, X esconde/mostra, B cancela.

## Desempenho / memória
Imagens carregadas em segundo plano, reduzidas e limitadas a 56 na memória (cache em disco no SD); textos em cache;
a tela só é redesenhada quando algo muda; download em thread própria com buffer grande e Snake a 30 quadros.
Imagem de fundo maior que ~8 megapixels é recusada (use 1280x720).
