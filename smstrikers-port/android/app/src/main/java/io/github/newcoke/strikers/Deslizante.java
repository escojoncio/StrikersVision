package io.github.newcoke.strikers;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

/**
 * O controle deslizante, também desenhado à mão, pelo mesmo motivo do
 * {@link Interruptor}.
 *
 * DUAS COISAS QUE NÃO SÃO ENFEITE:
 *
 *   - o valor aparece escrito ao lado do trilho e muda enquanto o dedo
 *     arrasta. Um deslizante sem número obriga a soltar e olhar o efeito para
 *     saber onde se está, e em "opacidade" ou "zona morta" o efeito é sutil;
 *   - o dedo não precisa pousar em cima do botão. Pousar em qualquer ponto do
 *     trilho já salta para lá. Um alvo de 20 pixels no meio de uma tela sensível
 *     ao toque é um alvo que se erra.
 */
class Deslizante extends View {

    interface AoMudar { void mudou(int valor); }

    private static final int AZUL = 0xFF0A84FF;    // o azul de sistema do iOS

    private final Paint tinta = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint texto = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF trilho = new RectF();

    private final int minimo, maximo;
    private final String sufixo;
    private int valor;
    private AoMudar ouvinte;

    Deslizante(Context ctx, int minimo, int maximo, int inicial, String sufixo) {
        super(ctx);
        this.minimo = minimo;
        this.maximo = maximo;
        this.sufixo = sufixo;
        this.valor = Math.max(minimo, Math.min(maximo, inicial));
        texto.setColor(0xFF8E8E93);
        texto.setTextAlign(Paint.Align.RIGHT);
    }

    void aoMudar(AoMudar o) { ouvinte = o; }

    @Override
    protected void onMeasure(int larguraSpec, int alturaSpec) {
        final float d = getResources().getDisplayMetrics().density;
        setMeasuredDimension(MeasureSpec.getSize(larguraSpec), Math.round(34 * d));
    }

    @Override
    protected void onDraw(Canvas canvas) {
        final float d = getResources().getDisplayMetrics().density;
        texto.setTextSize(13 * d);

        // O número fica à direita, com largura fixa reservada, para o trilho
        // não encolher e crescer enquanto o valor muda de "9%" para "100%".
        // A reserva do número, com folga. Foi medida duas vezes na tela e nas duas
        // o "100%" ainda encostava na borda do painel: em vez de estimar de novo,
        // sobra espaço e o texto é desenhado recuado da direita, não colado nela.
        final float reserva = 64 * d;
        final float folgaDireita = 8 * d;
        final float fim = getWidth() - reserva;
        final float cy = getHeight() / 2f;
        final float alt = 4 * d;
        final float fraccao = (valor - minimo) / (float) (maximo - minimo);

        trilho.set(0, cy - alt / 2, fim, cy + alt / 2);
        tinta.setColor(0xFF3A3A3C);
        canvas.drawRoundRect(trilho, alt, alt, tinta);

        trilho.set(0, cy - alt / 2, fraccao * fim, cy + alt / 2);
        tinta.setColor(AZUL);
        canvas.drawRoundRect(trilho, alt, alt, tinta);

        // O puck limitado ao trilho: no minimo e no maximo ele ficaria metade
        // para fora, e meia bolinha cortada na borda le-se como defeito.
        final float raioPuck = 11 * d;
        final float cx = Math.max(raioPuck, Math.min(fim - raioPuck, fraccao * fim));
        tinta.setColor(0x40000000);
        canvas.drawCircle(cx, cy + d, raioPuck, tinta);
        tinta.setColor(Color.WHITE);
        canvas.drawCircle(cx, cy, raioPuck, tinta);

        final Paint.FontMetrics fm = texto.getFontMetrics();
        canvas.drawText(valor + sufixo, getWidth() - folgaDireita,
                        cy - (fm.ascent + fm.descent) / 2f, texto);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        // Segurar o pai: sem isto, arrastar na horizontal dentro de uma lista
        // que rola faz a lista roubar o gesto no primeiro milímetro.
        getParent().requestDisallowInterceptTouchEvent(true);

        final float d = getResources().getDisplayMetrics().density;
        final float fim = Math.max(1f, getWidth() - 64 * d);
        final float f = Math.max(0f, Math.min(1f, e.getX() / fim));
        final int novo = Math.round(minimo + f * (maximo - minimo));
        if (novo != valor) {
            valor = novo;
            invalidate();
            if (ouvinte != null) ouvinte.mudou(valor);
        }
        return true;
    }
}
