package io.github.newcoke.strikers;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

/**
 * O launcher.
 *
 * <p>Ele responde a uma pergunta e oferece três coisas: onde está o disco, e daí
 * jogar, configurar ou trocar o driver de vídeo. O jogo em si diria a primeira
 * parte perfeitamente bem — o DVDInit lista todos os caminhos que tentou e põe
 * num quadro de mensagem — mas diria depois de uma tela preta e de um processo
 * que então encerra, o que num celular se lê como travamento. Então a pergunta é
 * feita antes de qualquer coisa ser carregada.
 */
public class SetupActivity extends Activity {

    private TextView estado;
    private TextView caminho;
    private TextView jogar;
    private TextView permissao;
    private TextView idioma;
    private Preferencias prefs;

    /**
     * UMA VEZ POR PROCESSO, e não uma por onCreate.
     *
     * <p>A abertura é para quando o aplicativo abre. O onCreate também roda ao
     * voltar de uma troca de idioma, que chama recreate(), e ver a bandeira de
     * novo logo depois de mexer numa configuração não parece uma abertura —
     * parece o aplicativo ter reiniciado sozinho.
     */
    private static boolean jaSaudou;

    @Override
    protected void attachBaseContext(Context base) {
        // Antes de qualquer getString desta Activity. Ver Idiomas.envolver.
        super.attachBaseContext(Idiomas.envolver(base));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // DEITADO DOS DOIS LADOS, agora que a tela existe.
        //
        // O manifesto pede "landscape", que é fixo, para o sistema poder desenhar
        // a janela de pré-visualização já girada -- sem isso ela sai em pé e a
        // tela gira depois, que é o lampejo de retrato ao abrir. Mas fixo também
        // quer dizer um lado só, e quem pega o celular ao contrário ficaria com o
        // launcher de cabeça para baixo. Relaxar aqui devolve os dois lados, e a
        // pré-visualização já passou.
        setRequestedOrientation(
                android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);

        prefs = new Preferencias(this);
        // setContentView ANTES de semBarras: o getInsetsController() da janela
        // passa pela decor view, e ela só existe depois que há um conteúdo.
        // Invertido, é um NullPointerException no onCreate.
        setContentView(montar());
        semBarras();
        saudar();
    }

    /** A bandeira, por cima de tudo, na primeira vez que esta tela aparece. */
    private void saudar() {
        if (jaSaudou) return;
        jaSaudou = true;

        final FrameLayout raiz = (FrameLayout) getWindow().getDecorView()
                .findViewById(android.R.id.content);
        final Bandeira[] bandeira = new Bandeira[1];
        bandeira[0] = new Bandeira(this, () -> {
            // Removida da hierarquia quando termina: ela redesenha a cada
            // quadro, e uma view invisível que continua pedindo quadros é uma
            // animação rodando para ninguém.
            if (bandeira[0] != null && bandeira[0].getParent() != null) {
                raiz.removeView(bandeira[0]);
            }
        });
        raiz.addView(bandeira[0], new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    private View montar() {
        final Context ctx = this;

        LinearLayout capa = new LinearLayout(ctx);
        capa.setOrientation(LinearLayout.VERTICAL);
        capa.setGravity(Gravity.CENTER_VERTICAL);
        capa.setPadding(Vitrine.dp(ctx, 34), Vitrine.dp(ctx, 28),
                        Vitrine.dp(ctx, 16), Vitrine.dp(ctx, 28));

        LinearLayout corpo = new LinearLayout(ctx);
        corpo.setOrientation(LinearLayout.VERTICAL);
        corpo.setPadding(Vitrine.dp(ctx, 8), Vitrine.dp(ctx, 28),
                         Vitrine.dp(ctx, 28), Vitrine.dp(ctx, 28));

        TextView titulo = new TextView(ctx);
        titulo.setText(R.string.app_name);
        titulo.setTextColor(Vitrine.TEXTO);
        titulo.setTextSize(26f);
        titulo.setTypeface(Typeface.create("sans-serif", Typeface.BOLD));
        titulo.setLineSpacing(0f, 0.95f);
        capa.addView(titulo);

        TextView subtitulo = new TextView(ctx);
        subtitulo.setText(R.string.app_subtitle);
        subtitulo.setTextColor(Vitrine.TEXTO_2);
        subtitulo.setTextSize(13f);
        subtitulo.setPadding(0, Vitrine.dp(ctx, 4), 0, 0);
        capa.addView(subtitulo);

        // --- o estado do disco ------------------------------------------------
        corpo.addView(Vitrine.secao(ctx, getString(R.string.sec_game_data)));
        LinearLayout disco = Vitrine.painel(ctx);

        estado = new TextView(ctx);
        estado.setTextColor(Vitrine.TEXTO);
        estado.setTextSize(13f);
        estado.setLineSpacing(Vitrine.dp(ctx, 3), 1f);
        estado.setPadding(Vitrine.dp(ctx, 20), Vitrine.dp(ctx, 16),
                          Vitrine.dp(ctx, 20), Vitrine.dp(ctx, 8));
        disco.addView(estado);

        caminho = new TextView(ctx);
        caminho.setTextColor(Vitrine.TEXTO_2);
        caminho.setTextSize(11f);
        caminho.setTypeface(Typeface.MONOSPACE);
        // NAO selecionavel: um TextView selecionavel e focavel, e um ScrollView
        // rola sozinho ate o filho com foco -- a coluna abria no meio do painel
        // em vez de no topo. O caminho ja tem o botao de copiar ao lado.
        caminho.setPadding(Vitrine.dp(ctx, 20), 0, Vitrine.dp(ctx, 20), Vitrine.dp(ctx, 12));
        disco.addView(caminho);

        LinearLayout acoes = new LinearLayout(ctx);
        acoes.setOrientation(LinearLayout.HORIZONTAL);
        acoes.setPadding(Vitrine.dp(ctx, 16), 0, Vitrine.dp(ctx, 16), Vitrine.dp(ctx, 16));

        TextView copiar = Vitrine.botao(ctx, getString(R.string.copy_path),
                                        Vitrine.MATERIAL_ALT, false);
        copiar.setOnClickListener(v -> copiarCaminho());
        acoes.addView(copiar);

        permissao = Vitrine.botao(ctx, getString(R.string.grant_all_files),
                                  Vitrine.MATERIAL_ALT, false);
        permissao.setOnClickListener(v -> pedirAcesso());
        LinearLayout.LayoutParams lpp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lpp.leftMargin = Vitrine.dp(ctx, 10);
        acoes.addView(permissao, lpp);
        disco.addView(acoes);

        corpo.addView(disco, largura());

        // --- o que dá para fazer ----------------------------------------------
        corpo.addView(Vitrine.secao(ctx, getString(R.string.sec_app)));
        LinearLayout opcoes = Vitrine.painel(ctx);

        LinearLayout linhaConfig = Vitrine.linha(ctx, getString(R.string.settings),
                getString(R.string.settings_sub), seta(ctx));
        linhaConfig.setClickable(true);
        linhaConfig.setOnClickListener(v -> MenuConfig.abrir(ctx, prefs, null));
        opcoes.addView(linhaConfig);

        opcoes.addView(Vitrine.separador(ctx));

        LinearLayout linhaDriver = Vitrine.linha(ctx, getString(R.string.video_driver),
                getString(R.string.video_driver_sub), seta(ctx));
        linhaDriver.setClickable(true);
        linhaDriver.setOnClickListener(v -> startActivity(new Intent(ctx, TelaDrivers.class)));
        opcoes.addView(linhaDriver);

        opcoes.addView(Vitrine.separador(ctx));

        // O IDIOMA FICA AQUI, no launcher, e não dentro das configurações. As
        // configurações abrem por cima do jogo em execução, e trocar o idioma
        // remonta a tela — remontar uma tela que está por cima de uma partida é
        // pedir para algo dar errado. Aqui não há nada por baixo.
        idioma = new TextView(ctx);
        idioma.setTextColor(Vitrine.TEXTO_2);
        idioma.setTextSize(14f);
        LinearLayout linhaIdioma = Vitrine.linha(ctx, getString(R.string.language),
                null, idioma);
        linhaIdioma.setClickable(true);
        linhaIdioma.setOnClickListener(v -> escolherIdioma());
        opcoes.addView(linhaIdioma);

        corpo.addView(opcoes, largura());

        // --- jogar --------------------------------------------------------------
        jogar = Vitrine.botao(ctx, getString(R.string.play), Vitrine.AZUL, true);
        jogar.setOnClickListener(v -> comecar());
        LinearLayout.LayoutParams lpj = new LinearLayout.LayoutParams(
                Vitrine.dp(ctx, 180), ViewGroup.LayoutParams.WRAP_CONTENT);
        lpj.topMargin = Vitrine.dp(ctx, 28);
        capa.addView(jogar, lpj);

        TextView aviso = new TextView(ctx);
        aviso.setText(R.string.no_assets_included);
        aviso.setTextColor(Vitrine.TEXTO_3);
        aviso.setTextSize(10f);
        aviso.setPadding(0, Vitrine.dp(ctx, 26), Vitrine.dp(ctx, 16), 0);
        aviso.setLineSpacing(Vitrine.dp(ctx, 2), 1f);
        capa.addView(aviso);

        // DUAS COLUNAS, porque a tela e deitada.
        //
        // Numa coluna so, este conteudo cabe em um terco da largura e obriga a
        // rolar num aparelho que tem 2340 pixels de sobra ao lado. A identidade
        // e o botao ficam parados a esquerda, que e onde o polegar esquerdo
        // esta; as escolhas rolam a direita.
        ScrollView rolagem = new ScrollView(ctx);
        rolagem.addView(corpo);
        rolagem.setClipToPadding(false);

        LinearLayout colunas = new LinearLayout(ctx);
        colunas.setOrientation(LinearLayout.HORIZONTAL);
        colunas.addView(capa, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.MATCH_PARENT, 0.42f));
        colunas.addView(rolagem, new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.MATCH_PARENT, 0.58f));

        FrameLayout pilha = new FrameLayout(ctx);
        pilha.addView(Vitrine.fundo(ctx));
        pilha.addView(colunas, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        return pilha;
    }

    private LinearLayout.LayoutParams largura() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.leftMargin = lp.rightMargin = Vitrine.dp(this, 4);
        return lp;
    }

    private TextView seta(Context ctx) {
        TextView t = new TextView(ctx);
        t.setText("›");
        t.setTextColor(Vitrine.TEXTO_3);
        t.setTextSize(22f);
        return t;
    }

    @Override
    protected void onResume() {
        super.onResume();
        semBarras();
        // Relido toda vez, porque as duas saídas desta tela voltam para ela: a
        // tela de permissões do sistema, e um gerenciador de arquivos ou um PC
        // com o disco.
        atualizar();
    }

    private void atualizar() {
        final String onde = StrikersHome.homeDir(this).getAbsolutePath();
        caminho.setText(onde);
        idioma.setText(Idiomas.nomeDe(this, prefs.texto(Preferencias.IDIOMA_APP)));

        final String achado = StrikersHome.describeGameData(this);
        final boolean pronto = achado != null;
        final boolean naRaiz = StrikersHome.naRaiz(this);

        jogar.setEnabled(pronto);
        jogar.setAlpha(pronto ? 1f : 0.4f);

        // Os textos vêm de strings.xml, e com eles as marcações <b> que a
        // tradução carrega -- daí getText e não getString: getString descarta a
        // formatação e mostraria "files" sem o negrito, que é justamente a
        // palavra que alguém precisa reconhecer na pasta do disco.
        final CharSequence corpo = pronto
                ? getString(R.string.disc_found, achado)
                : getText(R.string.data_missing);
        final CharSequence extra = naRaiz
                ? ""
                : getString(R.string.need_all_files, StrikersHome.PASTA);

        estado.setText(android.text.TextUtils.concat(corpo, extra));

        // Oferecido enquanto a pasta não for a da raiz — e não só quando falta o
        // disco. A permissão deixou de ser um atalho para encontrar um arquivo:
        // é ela que decide ONDE fica a pasta do jogo.
        permissao.setVisibility(naRaiz ? View.GONE : View.VISIBLE);
    }

    /**
     * A lista de idiomas.
     *
     * <p>Escolher REMONTA A TELA, e não só troca os textos à mão: os rótulos
     * estão espalhados por painéis, botões e cabeçalhos montados em código, e
     * reescrever cada um deles daqui seria uma lista que esquece um item na
     * primeira vez que alguém acrescentar uma linha. recreate() passa de novo
     * pelo attachBaseContext, que é onde o idioma entra.
     */
    private void escolherIdioma() {
        final String atual = prefs.texto(Preferencias.IDIOMA_APP);
        int marcado = 0;
        for (int i = 0; i < Idiomas.CODIGOS.length; i++) {
            if (Idiomas.CODIGOS[i].equals(atual)) marcado = i;
        }
        new android.app.AlertDialog.Builder(
                    this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(R.string.language)
                .setSingleChoiceItems(Idiomas.nomes(this), marcado, (dlg, i) -> {
                    dlg.dismiss();
                    if (Idiomas.CODIGOS[i].equals(atual)) return;
                    prefs.definirTexto(Preferencias.IDIOMA_APP, Idiomas.CODIGOS[i]);
                    recreate();
                })
                .show();
    }

    private void comecar() {
        startActivity(new Intent(this, StrikersActivity.class));
    }

    private void pedirAcesso() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return;
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            // Alguns aparelhos não têm a tela por aplicativo, só a lista.
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    private void copiarCaminho() {
        final String onde = StrikersHome.homeDir(this).getAbsolutePath();
        ClipboardManager area = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (area != null) {
            area.setPrimaryClip(ClipData.newPlainText("strikers", onde));
            Toast.makeText(this, R.string.copied, Toast.LENGTH_SHORT).show();
        }
    }

    private void semBarras() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            getWindow().getAttributes().layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
    }
}
