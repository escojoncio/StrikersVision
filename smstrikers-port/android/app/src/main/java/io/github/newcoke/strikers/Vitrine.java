package io.github.newcoke.strikers;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * O vocabulário visual do aplicativo, num lugar só.
 *
 * <p>É deliberadamente pequeno: um fundo, um painel de vidro, uma linha de
 * lista, um cabeçalho de seção, um botão. Tudo o mais é composição disso. Uma
 * tela que precise de um elemento novo ganha um método aqui em vez de inventar
 * cores e medidas locais — é o que mantém duas telas parecendo o mesmo programa.
 *
 * <p>AS MEDIDAS SÃO AS DA APPLE, e não aproximações: 17sp na linha, 13sp no
 * cabeçalho, 22dp de canto no painel, 44dp de altura mínima de toque. Elas não
 * são sagradas — são um conjunto que já foi resolvido, e copiar um conjunto
 * coerente sai melhor do que escolher seis números soltos.
 */
final class Vitrine {

    private Vitrine() {}

    // Rótulos, na escala de cinzas do modo escuro da Apple.
    static final int TEXTO        = 0xFFFFFFFF;
    static final int TEXTO_2      = 0x99EBEBF5;
    static final int TEXTO_3      = 0x61EBEBF5;
    static final int SEPARADOR    = 0x1FFFFFFF;
    static final int AZUL         = 0xFF0A84FF;
    static final int VERDE        = 0xFF32D74B;
    static final int LARANJA      = 0xFFFF9F0A;

    // O material: uma camada clara muito leve sobre o fundo desfocado, e um
    // risco de meio pixel na borda. Sem o risco o painel parece recortado; com
    // ele parece apoiado.
    static final int MATERIAL     = 0x14FFFFFF;
    static final int MATERIAL_ALT = 0x0FFFFFFF;
    static final int BORDA        = 0x26FFFFFF;

    // A arte desfocada, uma vez por processo. Os paineis nao desfocam cada um o
    // seu pedaco: desfocam o MESMO bitmap, cada um recortando a parte que lhe
    // cabe. E o que faz dois paineis em alturas diferentes parecerem duas
    // janelas no mesmo vidro, em vez de dois adesivos.
    private static Bitmap fosco;

    static Bitmap fosco(Context ctx) {
        if (fosco == null) {
            Bitmap arte = arte(ctx, 320);
            if (arte != null) {
                fosco = Vidro.desfocar(arte, 200, 10);
                arte.recycle();
            }
        }
        return fosco;
    }

    static float d(Context ctx) {
        return ctx.getResources().getDisplayMetrics().density;
    }

    static int dp(Context ctx, float v) {
        return Math.round(v * d(ctx));
    }

    /** A arte de fundo, uma vez, no tamanho pedido. Null se não der. */
    static Bitmap arte(Context ctx, int larguraMax) {
        BitmapFactory.Options medida = new BitmapFactory.Options();
        medida.inJustDecodeBounds = true;
        BitmapFactory.decodeResource(ctx.getResources(), R.drawable.fundo, medida);
        int amostra = 1;
        while (medida.outWidth / (amostra * 2) >= larguraMax) amostra *= 2;
        BitmapFactory.Options opcoes = new BitmapFactory.Options();
        opcoes.inSampleSize = amostra;
        return BitmapFactory.decodeResource(ctx.getResources(), R.drawable.fundo, opcoes);
    }

    /**
     * O fundo da tela: a arte, cobrindo, com um véu por cima.
     *
     * <p>O VÉU NÃO É GOSTO. A arte é saturada e cheia de detalhe; texto branco
     * sobre ela é ilegível em metade da área. Escurecer é o que permite manter a
     * imagem inteira em vez de recortar um pedaço calmo dela.
     */
    static View fundo(Context ctx) {
        FrameLayout pilha = new FrameLayout(ctx);
        ImageView arte = new ImageView(ctx);
        arte.setScaleType(ImageView.ScaleType.CENTER_CROP);
        arte.setImageResource(R.drawable.fundo);
        pilha.addView(arte, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        // O VEU, EM DEGRADE E NAO CHAPADO. A arte e clara e agitada; um veu
        // uniforme forte o bastante para o texto apaga a imagem toda, e um fraco
        // deixa o texto ilegivel. Em degrade, o lado onde o texto vive fica
        // escuro e o outro continua sendo a arte.
        View veu = new View(ctx);
        veu.setBackground(new android.graphics.drawable.GradientDrawable(
                android.graphics.drawable.GradientDrawable.Orientation.LEFT_RIGHT,
                new int[]{0xF2000000, 0xD9000000, 0x8C000000}));
        pilha.addView(veu, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        return pilha;
    }

    /** Um painel de vidro: cantos arredondados, material claro, risco na borda. */
    static LinearLayout painel(Context ctx) {
        LinearLayout p = new LinearLayout(ctx) {
            private final Paint tinta = new Paint(Paint.ANTI_ALIAS_FLAG);
            private final RectF caixa = new RectF();
            private final android.graphics.Matrix matriz = new android.graphics.Matrix();
            private final int[] onde = new int[2];
            private android.graphics.BitmapShader trama;

            @Override
            protected void dispatchDraw(Canvas canvas) {
                final float raio = dp(getContext(), 16);
                caixa.set(0, 0, getWidth(), getHeight());
                tinta.setStyle(Paint.Style.FILL);

                // O FUNDO DESFOCADO, ALINHADO COM A TELA E NAO COM O PAINEL.
                //
                // A matriz leva o bitmap ao tamanho da tela e depois desloca
                // pela posicao deste painel nela. Sem o deslocamento, cada
                // painel mostraria a imagem inteira encolhida dentro de si, que
                // e o aspecto de um adesivo com uma foto -- nao o de um vidro.
                Bitmap b = fosco(getContext());
                if (b != null) {
                    if (trama == null) {
                        trama = new android.graphics.BitmapShader(
                                b, android.graphics.Shader.TileMode.CLAMP,
                                android.graphics.Shader.TileMode.CLAMP);
                    }
                    getLocationOnScreen(onde);
                    final android.util.DisplayMetrics dm =
                            getResources().getDisplayMetrics();
                    matriz.reset();
                    matriz.setScale((float) dm.widthPixels / b.getWidth(),
                                    (float) dm.heightPixels / b.getHeight());
                    matriz.postTranslate(-onde[0], -onde[1]);
                    trama.setLocalMatrix(matriz);
                    tinta.setShader(trama);
                    canvas.drawRoundRect(caixa, raio, raio, tinta);
                    tinta.setShader(null);
                }

                tinta.setColor(MATERIAL);
                canvas.drawRoundRect(caixa, raio, raio, tinta);
                final float meio = Math.max(1f, d(getContext()) * 0.5f);
                caixa.inset(meio / 2f, meio / 2f);
                tinta.setStyle(Paint.Style.STROKE);
                tinta.setStrokeWidth(meio);
                tinta.setColor(BORDA);
                canvas.drawRoundRect(caixa, raio, raio, tinta);
                super.dispatchDraw(canvas);
            }
        };
        p.setOrientation(LinearLayout.VERTICAL);
        p.setWillNotDraw(false);
        return p;
    }

    /** Cabeçalho de seção: pequeno, em caixa alta, com espaço antes. */
    static TextView secao(Context ctx, String s) {
        TextView t = new TextView(ctx);
        t.setText(s.toUpperCase());
        t.setTextColor(TEXTO_3);
        t.setTextSize(11f);
        t.setLetterSpacing(0.06f);
        t.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        t.setPadding(dp(ctx, 16), dp(ctx, 18), dp(ctx, 16), dp(ctx, 6));
        return t;
    }

    /**
     * Uma linha de lista: rótulo à esquerda, um controle à direita, e uma
     * explicação opcional por baixo.
     *
     * <p>A explicação fica DENTRO da linha e não numa camada de ajuda: quem lê
     * "zona morta" e não sabe o que é, não vai procurar — vai arrastar e torcer.
     */
    static LinearLayout linha(Context ctx, String rotulo, String ajuda, View controle) {
        LinearLayout fora = new LinearLayout(ctx);
        fora.setOrientation(LinearLayout.VERTICAL);
        fora.setPadding(dp(ctx, 16), dp(ctx, 10), dp(ctx, 16), dp(ctx, 10));

        LinearLayout topo = new LinearLayout(ctx);
        topo.setOrientation(LinearLayout.HORIZONTAL);
        topo.setGravity(Gravity.CENTER_VERTICAL);
        topo.setMinimumHeight(dp(ctx, 28));

        TextView t = new TextView(ctx);
        t.setText(rotulo);
        t.setTextColor(TEXTO);
        t.setTextSize(14f);
        LinearLayout.LayoutParams cresce = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        topo.addView(t, cresce);

        if (controle != null) topo.addView(controle);
        fora.addView(topo, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        if (ajuda != null && !ajuda.isEmpty()) {
            TextView a = new TextView(ctx);
            a.setText(ajuda);
            a.setTextColor(TEXTO_2);
            a.setTextSize(11f);
            a.setPadding(0, dp(ctx, 2), dp(ctx, 8), 0);
            fora.addView(a);
        }
        return fora;
    }

    /** Uma linha que ocupa a largura toda, com um controle por baixo do rótulo. */
    static LinearLayout linhaLarga(Context ctx, String rotulo, String ajuda, View controle) {
        LinearLayout fora = new LinearLayout(ctx);
        fora.setOrientation(LinearLayout.VERTICAL);
        fora.setPadding(dp(ctx, 16), dp(ctx, 10), dp(ctx, 16), dp(ctx, 10));

        TextView t = new TextView(ctx);
        t.setText(rotulo);
        t.setTextColor(TEXTO);
        t.setTextSize(14f);
        fora.addView(t);

        if (controle != null) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.topMargin = dp(ctx, 6);
            fora.addView(controle, lp);
        }
        if (ajuda != null && !ajuda.isEmpty()) {
            TextView a = new TextView(ctx);
            a.setText(ajuda);
            a.setTextColor(TEXTO_2);
            a.setTextSize(11f);
            a.setPadding(0, dp(ctx, 2), 0, 0);
            fora.addView(a);
        }
        return fora;
    }

    /** O risco fino entre linhas, recuado como na lista da Apple. */
    static View separador(Context ctx) {
        View v = new View(ctx);
        v.setBackgroundColor(SEPARADOR);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, dp(ctx, 0.5f)));
        lp.leftMargin = dp(ctx, 16);
        v.setLayoutParams(lp);
        return v;
    }

    /** Um botão cheio, da cor pedida, com o texto centrado. */
    static TextView botao(Context ctx, String rotulo, final int cor, boolean forte) {
        TextView b = new TextView(ctx) {
            private final Paint tinta = new Paint(Paint.ANTI_ALIAS_FLAG);
            private final RectF caixa = new RectF();

            @Override
            protected void onDraw(Canvas canvas) {
                caixa.set(0, 0, getWidth(), getHeight());
                final float raio = getHeight() / 2f;
                tinta.setColor(isPressed() ? escurecer(cor) : cor);
                canvas.drawRoundRect(caixa, raio, raio, tinta);
                super.onDraw(canvas);
            }
        };
        b.setText(rotulo);
        b.setGravity(Gravity.CENTER);
        b.setTextColor(forte ? Color.WHITE : TEXTO);
        b.setTextSize(14f);
        b.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        b.setPadding(dp(ctx, 20), dp(ctx, 10), dp(ctx, 20), dp(ctx, 10));
        b.setClickable(true);
        b.setWillNotDraw(false);
        return b;
    }

    private static int escurecer(int cor) {
        final int a = cor >>> 24;
        final int r = (cor >> 16 & 0xFF) * 72 / 100;
        final int g = (cor >> 8 & 0xFF) * 72 / 100;
        final int b = (cor & 0xFF) * 72 / 100;
        return a << 24 | r << 16 | g << 8 | b;
    }
}
