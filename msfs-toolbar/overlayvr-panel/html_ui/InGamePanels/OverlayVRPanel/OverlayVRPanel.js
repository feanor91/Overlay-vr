// Bouton de barre d'outils : demande a l'application de la barre d'etat OverlayVR (127.0.0.1:47321) d'ouvrir / fermer
// l'editeur de zones integre a la couche OpenXR.
class IngamePanelOverlayVR extends TemplateElement {
    constructor() {
        super();
    }
    connectedCallback() {
        super.connectedCallback();
        this.status = this.querySelector("#ovr-status");
        const button = this.querySelector("#ovr-toggle");
        if (button) {
            button.addEventListener("click", () => this.toggle());
        }
    }
    setStatus(text) {
        if (this.status) this.status.textContent = text;
    }
    toggle() {
        this.setStatus("Envoi...");
        fetch("http://127.0.0.1:47321/toggle")
            .then((r) => r.text())
            .then((t) => this.setStatus(t === "ok" ? "Éditeur basculé" : "Aucun jeu avec la couche détecté"))
            .catch(() => this.setStatus("Application OverlayVR (barre d'état) injoignable"));
    }
}
window.customElements.define("ingamepanel-overlayvr", IngamePanelOverlayVR);
checkAutoload();
