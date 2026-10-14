package io.github.newcoke.strikers;

import android.graphics.Bitmap;

/**
 * Desfoca uma imagem, uma vez, para servir de vidro fosco ao overlay.
 *
 * POR QUE NAO O DESFOQUE DO SISTEMA
 * ---------------------------------
 * `RenderEffect.createBlurEffect` so existe da API 31 para cima, e desfoca o
 * conteudo da PROPRIA View - nao o que esta por tras dela, que e o que um vidro
 * fosco mostra. `Window.setBackgroundBlurRadius` desfoca por tras da janela
 * inteira, nao por forma. Nenhum dos dois faz o que aqui e preciso.
 *
 * Como o fundo e uma imagem parada, basta desfoca-lo UMA vez ao arrancar e
 * depois cada forma recorta esse resultado a si propria. Custo por quadro:
 * zero.
 *
 * O ALGORITMO E UMA CAIXA DE TRES PASSAGENS, e nao um gaussiano a serio, porque
 * tres caixas seguidas ja sao visualmente indistinguiveis de um gaussiano (e o
 * teorema do limite central a trabalhar a nosso favor) e custam uma fraccao.
 * A imagem e reduzida antes: desfocar e passar uma media larga, e uma media
 * larga sobre pixels que vao ser esticados outra vez e trabalho deitado fora.
 */
final class Vidro {

    private Vidro() {}

    /**
     * @param origem  imagem de fundo, em tamanho cheio
     * @param largura largura do resultado (o resto sai por proporcao)
     * @param raio    raio do desfoque, em pixels do resultado
     */
    static Bitmap desfocar(Bitmap origem, int largura, int raio) {
        final int altura = Math.max(1,
                Math.round(largura * (float) origem.getHeight() / origem.getWidth()));
        final Bitmap pequena = Bitmap.createScaledBitmap(origem, largura, altura, true);

        final int[] px = new int[largura * altura];
        pequena.getPixels(px, 0, largura, 0, 0, largura, altura);
        if (pequena != origem) pequena.recycle();

        final int[] tmp = new int[px.length];
        for (int passagem = 0; passagem < 3; passagem++) {
            caixaHorizontal(px, tmp, largura, altura, raio);
            caixaVertical(tmp, px, largura, altura, raio);
        }

        // UM ESCURECIMENTO LIGEIRO, e nao por gosto: o vidro do overlay tem
        // texto branco por cima, e sobre a rocha clara da referencia o branco
        // sobre branco desaparece. Escurecer o vidro mantem o contraste sem
        // precisar de contornos grossos.
        for (int i = 0; i < px.length; i++) {
            final int c = px[i];
            px[i] = 0xFF000000
                  | (((c >> 16 & 0xFF) * 70 / 100) << 16)
                  | (((c >> 8  & 0xFF) * 70 / 100) << 8)
                  |  ((c       & 0xFF) * 70 / 100);
        }

        return Bitmap.createBitmap(px, largura, altura, Bitmap.Config.ARGB_8888);
    }

    /** Media movel numa linha: soma o que entra, tira o que sai. */
    private static void caixaHorizontal(int[] dentro, int[] fora, int w, int h,
                                        int raio) {
        final int janela = raio * 2 + 1;
        for (int y = 0; y < h; y++) {
            final int linha = y * w;
            int r = 0, g = 0, b = 0;
            // Encher a janela inicial, com as bordas repetidas: sem isso o
            // desfoque escurece nos cantos, porque estaria a somar preto.
            for (int k = -raio; k <= raio; k++) {
                final int c = dentro[linha + clamp(k, w)];
                r += c >> 16 & 0xFF; g += c >> 8 & 0xFF; b += c & 0xFF;
            }
            for (int x = 0; x < w; x++) {
                fora[linha + x] = 0xFF000000 | (r / janela) << 16
                                | (g / janela) << 8 | (b / janela);
                final int sai = dentro[linha + clamp(x - raio, w)];
                final int entra = dentro[linha + clamp(x + raio + 1, w)];
                r += (entra >> 16 & 0xFF) - (sai >> 16 & 0xFF);
                g += (entra >> 8 & 0xFF) - (sai >> 8 & 0xFF);
                b += (entra & 0xFF) - (sai & 0xFF);
            }
        }
    }

    private static void caixaVertical(int[] dentro, int[] fora, int w, int h,
                                      int raio) {
        final int janela = raio * 2 + 1;
        for (int x = 0; x < w; x++) {
            int r = 0, g = 0, b = 0;
            for (int k = -raio; k <= raio; k++) {
                final int c = dentro[clamp(k, h) * w + x];
                r += c >> 16 & 0xFF; g += c >> 8 & 0xFF; b += c & 0xFF;
            }
            for (int y = 0; y < h; y++) {
                fora[y * w + x] = 0xFF000000 | (r / janela) << 16
                                | (g / janela) << 8 | (b / janela);
                final int sai = dentro[clamp(y - raio, h) * w + x];
                final int entra = dentro[clamp(y + raio + 1, h) * w + x];
                r += (entra >> 16 & 0xFF) - (sai >> 16 & 0xFF);
                g += (entra >> 8 & 0xFF) - (sai >> 8 & 0xFF);
                b += (entra & 0xFF) - (sai & 0xFF);
            }
        }
    }

    private static int clamp(int v, int limite) {
        return v < 0 ? 0 : (v >= limite ? limite - 1 : v);
    }
}
