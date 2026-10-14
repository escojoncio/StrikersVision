package io.github.newcoke.strikers;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
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

import java.util.List;

/**
 * A tela de drivers de vídeo.
 *
 * <p>O jogo desenha por Vulkan, e no Android o Vulkan é o driver que o
 * fabricante pôs no aparelho. O Turnip é o driver aberto do Mesa para GPUs
 * Adreno, e em aparelhos cujo driver de fábrica é antigo ou tem defeitos ele
 * costuma ser melhor. Em aparelho novo, com driver recente, costuma não mudar
 * nada — e a tela diz isso, em vez de vender uma melhoria que não vem.
 */
public class TelaDrivers extends Activity {

    private static final int PEDIDO_ZIP = 4001;

    private LinearLayout lista;
    private Preferencias prefs;
    private final Handler principal = new Handler(Looper.getMainLooper());

    @Override
    protected void attachBaseContext(Context base) {
        // O idioma escolhido no launcher, antes de qualquer getString. Ver
        // Idiomas.envolver.
        super.attachBaseContext(Idiomas.envolver(base));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = new Preferencias(this);
        setContentView(montar());
        semBarras();
    }

    private View montar() {
        LinearLayout corpo = new LinearLayout(this);
        corpo.setOrientation(LinearLayout.VERTICAL);
        corpo.setPadding(Vitrine.dp(this, 16), Vitrine.dp(this, 28),
                         Vitrine.dp(this, 16), Vitrine.dp(this, 36));

        TextView titulo = new TextView(this);
        titulo.setText(R.string.video_driver);
        titulo.setTextColor(Vitrine.TEXTO);
        titulo.setTextSize(26f);
        titulo.setTypeface(Typeface.create("sans-serif", Typeface.BOLD));
        titulo.setPadding(Vitrine.dp(this, 16), 0, 0, 0);
        corpo.addView(titulo);

        TextView explicacao = new TextView(this);
        explicacao.setText(R.string.drivers_intro);
        explicacao.setTextColor(Vitrine.TEXTO_2);
        explicacao.setTextSize(13f);
        explicacao.setPadding(Vitrine.dp(this, 16), Vitrine.dp(this, 8),
                              Vitrine.dp(this, 16), 0);
        corpo.addView(explicacao);

        lista = new LinearLayout(this);
        lista.setOrientation(LinearLayout.VERTICAL);
        corpo.addView(lista, largura());

        TextView voltar = Vitrine.botao(this, getString(R.string.done), Vitrine.AZUL, true);
        voltar.setOnClickListener(v -> finish());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                Vitrine.dp(this, 180), ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.gravity = Gravity.CENTER_HORIZONTAL;
        lp.topMargin = Vitrine.dp(this, 24);
        corpo.addView(voltar, lp);

        ScrollView rolagem = new ScrollView(this);
        rolagem.addView(corpo);

        FrameLayout pilha = new FrameLayout(this);
        pilha.addView(Vitrine.fundo(this));
        pilha.addView(rolagem, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        // O recorte da câmera é físico e não some com as barras: 94 pixels na
        // esquerda com o aparelho deitado. O fundo passa por baixo dele, a lista
        // não.
        rolagem.setOnApplyWindowInsetsListener((alvo, insets) -> {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                final android.graphics.Insets i = insets.getInsets(
                        WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                alvo.setPadding(i.left, i.top, i.right, i.bottom);
            } else {
                alvo.setPadding(insets.getSystemWindowInsetLeft(),
                                insets.getSystemWindowInsetTop(),
                                insets.getSystemWindowInsetRight(),
                                insets.getSystemWindowInsetBottom());
            }
            return insets;
        });
        return pilha;
    }

    private LinearLayout.LayoutParams largura() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.leftMargin = lp.rightMargin = Vitrine.dp(this, 4);
        return lp;
    }

    @Override
    protected void onResume() {
        super.onResume();
        semBarras();
        recarregar();
    }

    private void recarregar() {
        lista.removeAllViews();

        // --- instalados -------------------------------------------------------
        lista.addView(Vitrine.secao(this, getString(R.string.sec_inuse)));
        LinearLayout painel = Vitrine.painel(this);

        final String escolhido = prefs.texto(Preferencias.DRIVER);
        painel.addView(linhaDriver(getString(R.string.driver_system),
                getString(R.string.driver_system_sub), "", escolhido.isEmpty(), null));

        List<Drivers.Driver> instalados = Drivers.lista(this);
        for (Drivers.Driver d : instalados) {
            painel.addView(Vitrine.separador(this));
            painel.addView(linhaDriver(d.nome, d.descricao, d.id,
                                       d.id.equals(escolhido), d));
        }
        lista.addView(painel, largura());

        if (!instalados.isEmpty()) {
            nota(getString(R.string.drivers_restart_note));
        }

        // --- importar ---------------------------------------------------------
        lista.addView(Vitrine.secao(this, getString(R.string.sec_add)));
        LinearLayout add = Vitrine.painel(this);

        LinearLayout importar = Vitrine.linha(this, getString(R.string.import_zip),
                getString(R.string.import_zip_sub), seta());
        importar.setClickable(true);
        importar.setOnClickListener(v -> escolherZip());
        add.addView(importar);

        for (Drivers.Fonte f : Drivers.fontes(this)) {
            add.addView(Vitrine.separador(this));
            LinearLayout linha = Vitrine.linha(this, f.nome,
                    f.descricao + "\n" + f.url, seta());
            linha.setClickable(true);
            linha.setOnClickListener(v -> baixar(f));
            add.addView(linha);
        }
        lista.addView(add, largura());
        nota(getString(R.string.drivers_download_note));
    }

    private void nota(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextColor(Vitrine.TEXTO_3);
        t.setTextSize(11f);
        t.setPadding(Vitrine.dp(this, 16), Vitrine.dp(this, 8), Vitrine.dp(this, 16), 0);
        lista.addView(t);
    }

    private TextView seta() {
        TextView t = new TextView(this);
        t.setText("›");
        t.setTextColor(Vitrine.TEXTO_3);
        t.setTextSize(22f);
        return t;
    }

    /** Uma linha de driver: escolher tocando, remover no toque longo. */
    private View linhaDriver(String nome, String ajuda, final String id,
                             boolean ativo, final Drivers.Driver driver) {
        TextView marca = new TextView(this);
        marca.setText(ativo ? "✓" : "");
        marca.setTextColor(Vitrine.AZUL);
        marca.setTextSize(18f);

        LinearLayout linha = Vitrine.linha(this, nome, ajuda, marca);
        linha.setClickable(true);
        linha.setOnClickListener(v -> {
            prefs.definirTexto(Preferencias.DRIVER, id);
            recarregar();
        });
        if (driver != null) {
            linha.setOnLongClickListener(v -> {
                Drivers.remover(this, driver);
                if (id.equals(prefs.texto(Preferencias.DRIVER))) {
                    prefs.definirTexto(Preferencias.DRIVER, "");
                }
                Toast.makeText(this, R.string.driver_removed, Toast.LENGTH_SHORT).show();
                recarregar();
                return true;
            });
        }
        return linha;
    }

    private void escolherZip() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try {
            startActivityForResult(i, PEDIDO_ZIP);
        } catch (Exception e) {
            Toast.makeText(this, R.string.no_file_picker, Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onActivityResult(int pedido, int resultado, Intent dados) {
        super.onActivityResult(pedido, resultado, dados);
        if (pedido != PEDIDO_ZIP || resultado != RESULT_OK || dados == null) return;
        final Uri uri = dados.getData();
        if (uri == null) return;
        Toast.makeText(this, R.string.installing, Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            final Drivers.Driver d = Drivers.importar(this, uri);
            principal.post(() -> concluir(d));
        }, "importar-driver").start();
    }

    private void baixar(final Drivers.Fonte fonte) {
        Toast.makeText(this, getString(R.string.downloading, fonte.nome),
                       Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            final Drivers.Driver d = Drivers.baixar(this, fonte);
            principal.post(() -> concluir(d));
        }, "baixar-driver").start();
    }

    private void concluir(Drivers.Driver d) {
        if (d == null) {
            Toast.makeText(this, R.string.driver_failed, Toast.LENGTH_LONG).show();
            return;
        }
        prefs.definirTexto(Preferencias.DRIVER, d.id);
        Toast.makeText(this, getString(R.string.driver_installed, d.nome),
                       Toast.LENGTH_LONG).show();
        recarregar();
    }

    private void semBarras() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                // AS DUAS, e não só a de status. Deitado, a barra de navegação
                // fica de pé na lateral direita e cai justamente em cima da
                // coluna de drivers e do botão de concluir.
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
