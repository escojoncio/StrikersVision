package io.github.newcoke.strikers;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.view.MotionEvent;
import android.view.View;

/**
 * A abertura: a bandeira do Brasil tremulando, com a assinatura do porte.
 *
 * <p>É DESENHADA, e não uma imagem. Uma bandeira em PNG teria de vir em várias
 * resoluções para não ficar borrada num aparelho de 1440 linhas, e mesmo assim
 * só poderia tremular deformando pixels já achatados. Desenhada em coordenadas
 * da própria bandeira — 20 de largura por 14 de altura, que são as proporções
 * oficiais — ela sai nítida em qualquer tela e o pano pode ondular de verdade.
 *
 * <p>COMO O PANO ONDULA: o desenho vai uma vez para um bitmap e depois é
 * redesenhado por {@link Canvas#drawBitmapMesh}, que estica esse bitmap sobre
 * uma grade de vértices. A cada quadro a grade recebe uma onda senoidal que
 * corre da haste para a ponta, presa na haste (amplitude zero em x=0) e solta na
 * outra borda — que é como um pano presa de um lado se comporta. Por cima passa
 * um brilho, na mesma fase da onda, porque uma dobra que não muda de luz não
 * parece uma dobra, parece um desenho torto.
 *
 * <p>SOBRE A FIDELIDADE: as quatro figuras — campo, losango, disco e faixa — e
 * as suas medidas são as da lei. O CÉU NÃO É: são 27 estrelas em cinco tamanhos,
 * com o Cruzeiro do Sul reconhecível e a Spica sozinha abaixo da faixa, mas as
 * posições são arranjadas e não a carta celeste levantada do decreto. Está dito
 * aqui porque uma aproximação anunciada é uma escolha, e uma aproximação calada
 * é um erro.
 */
final class Bandeira extends View {

    // As cores oficiais (Pantone 356 C, 116 C, 280 C) em sRGB.
    private static final int VERDE    = 0xFF009B3A;
    private static final int AMARELO  = 0xFFFEDF00;
    private static final int AZUL     = 0xFF002776;
    private static final int BRANCO   = 0xFFFFFFFF;

    // A bandeira tem 20 de largura por 14 de altura. Todo o desenho abaixo fala
    // nessas unidades, e uma matriz de escala leva para pixels no fim.
    private static final float LARGURA = 20f, ALTURA = 14f;

    // O tempo, em milissegundos. Uma abertura que demora é uma abertura que
    // irrita na terceira vez; esta cabe no tempo em que o dedo ainda está
    // voltando do ícone.
    private static final long ENTRADA = 380L;
    private static final long TOTAL   = 2350L;
    private static final long SAIDA   = 420L;

    // A grade do pano. 28x18 são 522 vértices: o bastante para a onda ficar
    // curva em vez de facetada, e pouco o bastante para o cálculo caber com
    // folga entre dois quadros.
    private static final int COLUNAS = 28, LINHAS = 18;

    private final Paint tinta = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint texto = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final float[] vertices = new float[2 * (COLUNAS + 1) * (LINHAS + 1)];

    private Bitmap pano;
    private long comecou;
    private long pedidoDeSaida = -1L;
    private boolean avisou;
    private final Runnable pronto;

    Bandeira(Context ctx, Runnable pronto) {
        super(ctx);
        this.pronto = pronto;
        setClickable(true);
        texto.setTextAlign(Paint.Align.CENTER);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        // Pular. Quem já viu não precisa ver de novo, e uma abertura que não se
        // pode pular é a primeira coisa de que alguém reclama.
        if (e.getActionMasked() == MotionEvent.ACTION_DOWN) sair();
        return true;
    }

    private void sair() {
        if (pedidoDeSaida < 0) pedidoDeSaida = agora();
    }

    private long agora() {
        return android.os.SystemClock.uptimeMillis();
    }

    // -----------------------------------------------------------------------
    // O desenho da bandeira, uma vez
    // -----------------------------------------------------------------------

    /** O pano em pixels, desenhado uma vez no tamanho pedido. */
    private Bitmap desenhar(int larguraPx) {
        final int alturaPx = Math.max(1, Math.round(larguraPx * ALTURA / LARGURA));
        final Bitmap b = Bitmap.createBitmap(larguraPx, alturaPx, Bitmap.Config.ARGB_8888);
        final Canvas c = new Canvas(b);
        final float e = larguraPx / LARGURA;   // escala: unidades da bandeira -> pixels
        c.scale(e, e);

        final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);

        // O campo.
        p.setColor(VERDE);
        c.drawRect(0, 0, LARGURA, ALTURA, p);

        // O losango, a 1,7 de cada borda.
        final float m = 1.7f;
        Path losango = new Path();
        losango.moveTo(LARGURA / 2f, m);
        losango.lineTo(LARGURA - m, ALTURA / 2f);
        losango.lineTo(LARGURA / 2f, ALTURA - m);
        losango.lineTo(m, ALTURA / 2f);
        losango.close();
        p.setColor(AMARELO);
        c.drawPath(losango, p);

        // O disco, de raio 3,5 no centro.
        final float cx = LARGURA / 2f, cy = ALTURA / 2f, r = 3.5f;
        p.setColor(AZUL);
        c.drawCircle(cx, cy, r, p);

        // As estrelas vêm antes da faixa: a faixa passa por cima das que ficam
        // atrás dela, e é assim na bandeira.
        estrelas(c, p, cx, cy, r);

        // A FAIXA. Dois arcos de raios 8,0 e 8,5 com o mesmo centro, 6 acima do
        // centro do disco, recortados pelo disco. Os ângulos são folgados de
        // propósito — quem decide onde a faixa termina é o recorte, não uma
        // conta de interseção que teria de ser refeita a cada ajuste.
        c.save();
        Path disco = new Path();
        disco.addCircle(cx, cy, r, Path.Direction.CW);
        c.clipPath(disco);

        final float arcoCy = cy - 6f;
        final RectF fora = new RectF(cx - 8.5f, arcoCy - 8.5f, cx + 8.5f, arcoCy + 8.5f);
        final RectF dentro = new RectF(cx - 8.0f, arcoCy - 8.0f, cx + 8.0f, arcoCy + 8.0f);

        Path faixa = new Path();
        faixa.arcTo(fora, 115f, -50f, true);
        faixa.arcTo(dentro, 65f, 50f, false);
        faixa.close();
        p.setColor(BRANCO);
        c.drawPath(faixa, p);

        // ORDEM E PROGRESSO, acompanhando a curva.
        Path caminho = new Path();
        final RectF meio = new RectF(cx - 8.25f, arcoCy - 8.25f, cx + 8.25f, arcoCy + 8.25f);
        caminho.arcTo(meio, 113f, -46f, true);

        Paint lema = new Paint(Paint.ANTI_ALIAS_FLAG);
        lema.setColor(VERDE);
        lema.setTypeface(Typeface.create("sans-serif", Typeface.BOLD));
        lema.setTextSize(0.30f);
        lema.setTextAlign(Paint.Align.CENTER);
        // O texto é curto e a curva é longa: medido e centrado à mão, porque
        // drawTextOnPath alinha pelo começo do caminho e não pelo meio dele.
        final float largura = lema.measureText(getResources().getString(R.string.flag_motto));
        final android.graphics.PathMeasure pm = new android.graphics.PathMeasure(caminho, false);
        final float sobra = (pm.getLength() - largura) / 2f;
        lema.setTextAlign(Paint.Align.LEFT);
        c.drawTextOnPath(getResources().getString(R.string.flag_motto),
                         caminho, Math.max(0f, sobra), 0.10f, lema);

        c.restore();
        return b;
    }

    /**
     * O céu: 27 estrelas em cinco tamanhos.
     *
     * <p>As posições são relativas ao centro do disco, em raios (−1 a 1), com y
     * crescendo para baixo. A Spica — a última da tabela — é a única abaixo da
     * faixa, que é o que a bandeira diz do Pará.
     */
    private static final float[] CEU = {
        // x, y, tamanho (1 = maior, 5 = menor)
        //
        // A BORDA DE CIMA DA FAIXA passa por y ≈ 0,57 no meio e desce para
        // ≈ 0,43 nas pontas. Toda estrela daqui menos a última fica acima dessa
        // curva com folga: uma estrela meio coberta pela faixa não parece uma
        // escolha, parece um erro de posição -- e a bandeira tem exatamente uma
        // estrela abaixo da faixa, que é o que ela diz do Pará.
        -0.72f, -0.30f, 1,   // Procyon
        -0.55f,  0.16f, 1,   // Sirius
        -0.72f,  0.34f, 4,   // Cão Maior
        -0.46f,  0.36f, 4,
        -0.60f,  0.42f, 3,
        -0.34f,  0.44f, 5,
        -0.20f, -0.56f, 2,   // Canopus
         0.06f, -0.66f, 4,
        -0.06f, -0.40f, 3,
         0.22f, -0.52f, 5,
        -0.30f, -0.12f, 3,
        -0.10f, -0.06f, 4,
         0.10f, -0.20f, 5,
         0.46f, -0.44f, 5,
         0.56f, -0.18f, 2,   // Hidra
         0.70f, -0.02f, 4,
         0.62f,  0.22f, 3,
         0.46f,  0.34f, 5,
         0.16f,  0.04f, 1,   // Cruzeiro do Sul
         0.16f,  0.42f, 2,
        -0.02f,  0.24f, 2,
         0.34f,  0.24f, 3,
         0.16f,  0.24f, 5,
         0.02f,  0.46f, 4,   // Sigma Octantis, logo acima da faixa
        -0.16f,  0.46f, 4,   // Triângulo Austral
         0.30f,  0.44f, 5,
         0.00f,  0.86f, 5,   // Spica: a única abaixo da faixa
    };

    private void estrelas(Canvas c, Paint p, float cx, float cy, float r) {
        p.setColor(BRANCO);
        for (int i = 0; i < CEU.length; i += 3) {
            final float x = cx + CEU[i] * r;
            final float y = cy + CEU[i + 1] * r;
            // Os cinco tamanhos da lei, em unidades da bandeira.
            final float raio;
            switch ((int) CEU[i + 2]) {
                case 1:  raio = 0.150f; break;
                case 2:  raio = 0.125f; break;
                case 3:  raio = 0.105f; break;
                case 4:  raio = 0.085f; break;
                default: raio = 0.065f; break;
            }
            c.drawPath(estrela(x, y, raio), p);
        }
    }

    /** Uma estrela de cinco pontas, com a ponta para cima. */
    private static Path estrela(float cx, float cy, float raio) {
        Path s = new Path();
        final float dentro = raio * 0.42f;
        for (int i = 0; i < 10; i++) {
            final double a = -Math.PI / 2.0 + i * Math.PI / 5.0;
            final float rr = (i % 2 == 0) ? raio : dentro;
            final float x = cx + (float) (rr * Math.cos(a));
            final float y = cy + (float) (rr * Math.sin(a));
            if (i == 0) s.moveTo(x, y); else s.lineTo(x, y);
        }
        s.close();
        return s;
    }

    // -----------------------------------------------------------------------
    // A animação
    // -----------------------------------------------------------------------

    @Override
    protected void onDraw(Canvas canvas) {
        if (comecou == 0) comecou = agora();
        final long t = agora() - comecou;

        // O fim chega por tempo ou por toque, o que vier primeiro.
        if (pedidoDeSaida < 0 && t >= TOTAL) pedidoDeSaida = comecou + TOTAL;

        float opacidade = suave(Math.min(1f, t / (float) ENTRADA));
        if (pedidoDeSaida >= 0) {
            final float s = (agora() - pedidoDeSaida) / (float) SAIDA;
            if (s >= 1f) {
                // POR post() E NÃO DIRETO: quem espera este aviso tira esta view
                // da tela, e tirar uma view da hierarquia de dentro do próprio
                // desenho é mexer na lista de filhos que o pai está percorrendo.
                // O aviso vai para o fim da fila, depois deste quadro.
                if (!avisou) {
                    avisou = true;
                    if (pronto != null) post(pronto);
                }
                return;
            }
            opacidade *= 1f - suave(s);
        }

        final float w = getWidth(), h = getHeight();

        // O fundo: preto quase opaco, para a bandeira não competir com a arte do
        // launcher por baixo.
        tinta.setShader(null);
        tinta.setColor(Color.BLACK);
        tinta.setAlpha(Math.round(242 * opacidade));
        canvas.drawRect(0, 0, w, h, tinta);
        tinta.setAlpha(255);

        // A bandeira ocupa 56% da largura, ou o que a altura permitir.
        float lb = w * 0.56f;
        if (lb * ALTURA / LARGURA > h * 0.52f) lb = h * 0.52f * LARGURA / ALTURA;
        final float hb = lb * ALTURA / LARGURA;

        if (pano == null || pano.isRecycled()) {
            pano = desenhar(Math.min(1400, Math.max(320, Math.round(lb * 1.6f))));
        }

        // A entrada: entra um pouco maior e assenta. Um elemento que só aparece
        // parece um erro de desenho; um que assenta parece ter chegado.
        final float entrada = suave(Math.min(1f, t / (float) ENTRADA));
        final float escala = 1.06f - 0.06f * entrada;

        final float x0 = (w - lb * escala) / 2f;
        final float y0 = h * 0.40f - hb * escala / 2f;

        // A onda, em fase, e a amplitude caindo ao longo da abertura: começa
        // como um pano que acabou de ser sacudido e termina como um pano que
        // está só respirando.
        final float fase = t / 520f;
        final float decaimento = 1f - 0.62f * suave(Math.min(1f, t / (float) TOTAL));
        final float amplitude = hb * 0.085f * decaimento;

        int k = 0;
        for (int j = 0; j <= LINHAS; j++) {
            for (int i = 0; i <= COLUNAS; i++) {
                final float u = i / (float) COLUNAS;
                final float v = j / (float) LINHAS;
                // Presa na haste: a amplitude cresce do zero em u=0 até 1 na
                // ponta, e não linearmente -- um pano preso dobra pouco perto do
                // mastro e muito na ponta.
                final float presa = u * u * (3f - 2f * u);
                final float onda =
                        (float) (Math.sin((u * 2.6f - fase) * 2.0 * Math.PI)
                               + 0.42 * Math.sin((u * 4.3f - fase * 1.7f) * 2.0 * Math.PI));
                vertices[k++] = x0 + u * lb * escala;
                vertices[k++] = y0 + v * hb * escala
                              + amplitude * presa * onda
                              // a dobra também inclina as linhas, senão o pano
                              // parece uma persiana em vez de um tecido
                              + amplitude * presa * onda * (v - 0.5f) * 0.55f;
            }
        }

        tinta.setAlpha(Math.round(255 * opacidade));
        canvas.drawBitmapMesh(pano, COLUNAS, LINHAS, vertices, 0, null, 0, tinta);
        tinta.setAlpha(255);

        // O BRILHO, na mesma fase da onda. Sem ele a malha ondula e o pano
        // continua parecendo chapado: é a luz que muda numa dobra que informa
        // que existe uma dobra.
        brilho(canvas, x0, y0, lb * escala, hb * escala, fase, opacidade * decaimento);

        // A assinatura, entrando depois da bandeira.
        final float atraso = limitar((t - 620f) / 420f);
        if (atraso > 0f) assinatura(canvas, w, y0 + hb * escala, opacidade * suave(atraso));

        postInvalidateOnAnimation();
    }

    private void brilho(Canvas canvas, float x, float y, float w, float h,
                        float fase, float forca) {
        final float deslocamento = -(fase % 1f) * w / 2.6f;
        tinta.setShader(new LinearGradient(
                x + deslocamento, 0, x + deslocamento + w / 2.6f, 0,
                new int[]{0x00FFFFFF, 0x38FFFFFF, 0x00000000, 0x30000000, 0x00000000},
                new float[]{0f, 0.22f, 0.5f, 0.74f, 1f},
                Shader.TileMode.REPEAT));
        tinta.setAlpha(Math.round(255 * limitar(forca)));
        canvas.drawRect(x, y, x + w, y + h, tinta);
        tinta.setShader(null);
        tinta.setAlpha(255);
    }

    private void assinatura(Canvas canvas, float w, float base, float opacidade) {
        final float d = Vitrine.d(getContext());
        final int a = Math.round(255 * limitar(opacidade));

        texto.setTypeface(Typeface.create("sans-serif", Typeface.BOLD));
        texto.setTextSize(26 * d);
        texto.setLetterSpacing(0.34f);
        texto.setColor(AMARELO);
        texto.setAlpha(a);
        canvas.drawText(getResources().getString(R.string.flag_country),
                        w / 2f, base + 52 * d, texto);

        texto.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        texto.setTextSize(13 * d);
        texto.setLetterSpacing(0.02f);
        texto.setColor(Color.WHITE);
        texto.setAlpha(Math.round(a * 0.72f));
        canvas.drawText(getResources().getString(R.string.flag_credit),
                        w / 2f, base + 80 * d, texto);
    }

    /** Aceleração e desaceleração, para nada começar nem parar de estalo. */
    private static float suave(float t) {
        t = limitar(t);
        return t * t * (3f - 2f * t);
    }

    private static float limitar(float t) {
        return t < 0f ? 0f : (t > 1f ? 1f : t);
    }

    @Override
    protected void onDetachedFromWindow() {
        if (pano != null && !pano.isRecycled()) pano.recycle();
        pano = null;
        super.onDetachedFromWindow();
    }
}
