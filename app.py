from flask import Flask, render_template, request
from biobert_model import analyze_report

app = Flask(__name__)

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/analyze', methods=['POST'])
def analyze():
    report_text = request.form['report_text']
    result = analyze_report(report_text)
    return render_template('result.html', result=result, report_text=report_text)

if __name__ == '__main__':
    app.run(debug=True)
