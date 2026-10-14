package io.github.newcoke.strikers;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

/**
 * O interruptor de ligar/desligar, desenhado à mão.
 *
 * POR QUE NÃO O `Switch` DO ANDROID: o do sistema traz o visual do fabricante
 * — na Samsung ele é quadrado e azul, e no meio de um painel escuro com cantos
 * arredondados destoa na hora. Desenhar o trilho e o botão é meia dúzia de
 * linhas e fica igual em qualquer aparelho, que é o ponto de um app com
 * aparência própria.
 *
 * A ANIMAÇÃO É APROXIMAÇÃO EXPONENCIAL, não um salto: o botão caminha 28% da
 * distância que falta a cada quadro. É a mesma curva que o overlay usa ao
 * pressionar, e é o que separa "um controle" de "uma luz que pisca".
 */
class Interruptor extends View {

    interface AoMudar { void mudou(boolean ligado); }

    private static final int VERDE = 0xFF32D74B;   // o verde de sistema do iOS
    private static final int TRILHO_DESLIGADO = 0xFF3A3A3C;

    private final Paint tinta = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF trilho = new RectF();
    private boolean ligado;
    private float posicao;        // 0 = desligado, 1 = ligado
    private AoMudar ouvinte;

    Interruptor(Context ctx, boolean inicial) {
        super(ctx);
        ligado = inicial;
        posicao = inicial ? 1f : 0f;
        setClickable(true);
    }

    void aoMudar(AoMudar o) { ouvinte = o; }

    boolean estaLigado() { return ligado; }

    @Override
    protected void onMeasure(int larguraSpec, int alturaSpec) {
        final float d = getResources().getDisplayMetrics().density;
        setMeasuredDimension(Math.round(51 * d), Math.round(31 * d));
    }

    @Override
    protected void onDraw(Canvas canvas) {
        final float alvo = ligado ? 1f : 0f;
        posicao += (alvo - posicao) * 0.28f;
        if (Math.abs(alvo - posicao) > 0.002f) postInvalidateOnAnimation();
        else posicao = alvo;

        final float h = getHeight(), w = getWidth();
        final float r = h / 2f;

        trilho.set(0, 0, w, h);
        tinta.setColor(misturar(TRILHO_DESLIGADO, VERDE, posicao));
        canvas.drawRoundRect(trilho, r, r, tinta);

        // A pastilha branca, com uma sombra curta por baixo para descolar do
        // trilho. Sem ela o interruptor parece impresso.
        final float folga = h * 0.09f;
        final float raio = r - folga;
        final float cx = r + posicao * (w - h);
        tinta.setColor(0x33000000);
        canvas.drawCircle(cx, r + folga * 0.6f, raio, tinta);
        tinta.setColor(Color.WHITE);
        canvas.drawCircle(cx, r, raio, tinta);
    }

    private static int misturar(int a, int b, float t) {
        final int ra = a >> 16 & 0xFF, ga = a >> 8 & 0xFF, ba = a & 0xFF;
        final int rb = b >> 16 & 0xFF, gb = b >> 8 & 0xFF, bb = b & 0xFF;
        return 0xFF000000
             | (Math.round(ra + (rb - ra) * t) << 16)
             | (Math.round(ga + (gb - ga) * t) << 8)
             |  Math.round(ba + (bb - ba) * t);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (e.getActionMasked() == MotionEvent.ACTION_UP) {
            ligado = !ligado;
            invalidate();
            if (ouvinte != null) ouvinte.mudou(ligado);
        }
        return true;
    }
}
