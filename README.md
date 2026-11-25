Universal Recorder
==================

Gravador de ecrã para Windows. Guarda o vídeo em MP4, com o som do computador e do microfone.

Como usar
---------

Descarrega `Universal-Recorder-x64.exe` na [página de releases](https://github.com/gonbenvindo-debug/universal-recorder/releases/latest) e executa-o.
Prime `Ctrl+Shift+R` para começar a gravar e volta a premir para parar.
O ponto junto ao relógio fica vermelho enquanto gravas e amarelo quando paras.

Clica com o botão direito no ponto para escolher a pasta, a resolução ou os FPS.
Se tiveres mais de um monitor, também podes escolher qual gravar.

Compilar
--------

Com as ferramentas de C/C++ do Visual Studio e o Windows SDK 10.0.26100 ou posterior, executa o seguinte comando.

    build.cmd x64

O executável fica na mesma pasta.

Baseado no [wcap](https://github.com/mmozeiko/wcap/tree/aa25ccb806d7a6e1c0bfdcca863aabcd8e9badfa), de Martins Mozeiko. [Licença](LICENSE).
